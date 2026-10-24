// The graphics bridge: Direct3D 11 effects of mods over the game's frame (mml.h gfx_pass, MmlGfxFrame;
// docs/MODDING.md "Effects over the frame"). After mafia-ragdoll's gfx.cpp, its design kept, for any number of mods.
//
// The game's Direct3D 8 runs on Direct3D 9Ex through a wrapper (dxwrapper's d3d8to9 and D3d9to9Ex, as in the labs):
// the 9Ex device answers the game's device's QueryInterface. The bridge makes a Direct3D 11 device on the same
// adapter (matched by its LUID) and textures both share (made by 9Ex with a shared handle, opened by 11):
//
//   1. Before LS3D draws the mission's world (render.cpp's Render hook, the outermost call, the world's scene), the
//      frame's depth buffer is swapped for a readable one (INTZ) of its size.
//   2. At the scene's stage 1 (the world drawn, the HUD not yet; after the mods' RENDER_WORLD drawing), Direct3D 9
//      copies the colour so far and the depth as linear view depth (R32F) into shared textures, and waits for that.
//   3. Direct3D 11 clears the effects target (premultiplied RGBA16F) and runs every registered pass in order into it
//      (each behind its mod's fault guard), with GPU timestamps around each, and waits for the GPU.
//   4. Direct3D 9 blends the target over the frame (ONE, INVSRCALPHA) and gives back every state it touched.
//   5. After Render the game's own depth buffer is put back.
// On Direct3D 9Ex the devices wait for each other with event queries on the game's thread (9Ex has no keyed mutex and
// no fence: nothing else orders two devices). With no pass registered the bridge does nothing at all; its device is
// made at the first frame with a pass and kept (reloads do not remake it). When Direct3D 9 is DXVK's (its d3d9.dll
// beside the game: Vulkan underneath, whose shared handles Direct3D 11 cannot open) each shared texture is two, copied
// on DXVK's queue at the hand-overs (gfx_vk.cpp: Hand here), and two fences of Direct3D 11 shared with DXVK's device
// order the copies and the passes on the GPU: the game's thread waits for neither device (GpuSync).
//
// A temporal upscaler (gfx_upscale: DLSS) has the world drawn smaller: from GfxBeforeWorld to stage 1 every viewport the
// game sets on the frame's target is scaled to the upscaler's render size, so the world fills the top-left of the
// frame's targets at that size (the projection unchanged: the same picture, fewer pixels; the jitter in its pixels).
// At stage 1 the game's own viewport is put back: what LS3D draws after (its near pass, the lens flares) and the 2D pass
// are at the frame's size. The upscaler runs first among the passes and draws the whole frame from the smaller picture
// (else the bridge stretches it); the passes after it get the frame at its size (its picture, the depth and the motion
// vectors brought up to it).
//
// The frame's times (gfx.state timing; the log once a minute): the time between the game's Presents, of it the game
// thread's waits for the devices and its time in Present, and the GPU's: Direct3D 9's from the frame's first drawing to
// its Present (timestamp queries, read frames later) and the passes'.
//
// The frame's constants: the camera from the device's transforms at stage 1; the world's fog and sun from the
// game's own drawing: LS3D turns its fog off for what it draws last and enables its lights around the objects they
// light, so at stage 1 the device's state is not the world's. The device's SetRenderState, SetLight, LightEnable and
// draw calls are hooked once the bridge is on: each draw adds its primitives to the fog and the brightest directional
// light it is drawn with; the fog and the light most of the frame was drawn with up to stage 1 are the world's.
//
// Bridge methods (game thread): gfx.state (on/off and why, the adapter, the frame's size, the passes with their CPU
// and GPU times, the bridge's own times, the world's fog and sun as found), gfx.reset (after a failure: try again).
#include "host.h"
#include <d3d9.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>   // (ID3D11Device5::OpenSharedFence: the halves' shared fences)
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "json.hpp"
#include "gfx_vk.h"
#include "shaders/VsQuad.h"
#include "shaders/PsDepth.h"
#include "shaders/PsComposite.h"
#include "shaders/VsFullScreen.h"
#include "shaders/PsStretch.h"
#include "shaders/PsUpDepth.h"
#include "shaders/PsUpMotion.h"
#include "shaders/PsMotionAll.h"

using json = nlohmann::json;

namespace host {
namespace {

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

const D3DFORMAT kFmtIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
const float kFarDepth = 100000.0f;   // the linear depth written where nothing was drawn (the sky)
const double kWaitLimitMs = 250;     // a device that does not finish in this time: the frame goes without the passes
double g_waitLimitMs = -1;           // kWaitLimitMs, or MML_GFX_WAIT_MS from the environment (a test of the late frames)
double WaitLimitMs() {
    if (g_waitLimitMs < 0) {
        wchar_t b[32];
        DWORD n = GetEnvironmentVariableW(L"MML_GFX_WAIT_MS", b, 32);
        g_waitLimitMs = n && n < 32 ? _wtof(b) : kWaitLimitMs;
        if (g_waitLimitMs < 0) g_waitLimitMs = kWaitLimitMs;
        if (g_waitLimitMs != kWaitLimitMs) HLOG("gfx: the devices waited for %.1f ms (MML_GFX_WAIT_MS)", g_waitLimitMs);
        gfxvk::WaitLimit(g_waitLimitMs);
    }
    return g_waitLimitMs;
}
const double kLateGiveUpMs = 10000;  // the devices late this long, frame after frame: the bridge goes off
const unsigned kLateLogged = 10;     // the late runs written to the log (the rest only counted)

// Direct3D 11 and DXGI, loaded at the first frame with a pass (a game without effect mods never loads them).
using CreateDevice11Fn = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                           ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
using CreateFactory1Fn = HRESULT(WINAPI *)(REFIID, void **);
CreateDevice11Fn pD3D11CreateDevice;
CreateFactory1Fn pCreateDXGIFactory1;

struct Pass {
    MmlMod *mod;
    int handle, order;
    unsigned seq;
    MmlGfxPassFn fn;
    void *user;
    double cpuMs = 0, gpuMs = 0;   // smoothed
    bool inViews = false;          // gfx_pass_views: also over each scene view's picture
};
std::vector<Pass> g_passes;        // in order
int g_nextHandle = 1;
unsigned g_seq;

// ---- devices
void *g_d3d8;                      // the game's device the rest was made for
IDirect3DDevice9 *g_dev9;          // (9Ex under dxwrapper; DXVK's plain one under a d3d8to9 without 9Ex)
ID3D11Device *g_dev11;
ID3D11DeviceContext *g_ctx;
unsigned g_generation;
std::string g_state = "idle (no pass registered)", g_adapter;
bool g_failed;                     // set up or a frame failed: off until the device changes or gfx.reset
// A device late at a hand-over (a mission's load: the textures sent up, shaders made) is not a failure: the frame goes
// without the passes, the next frames too while that work runs (nothing shared is touched meanwhile), and the bridge
// goes off only when the devices stay late kLateGiveUpMs (seen 2026-10-22: one slow frame just after the freeride's
// load in a player's game under DXVK turned it off for the rest of the game).
bool g_pending9, g_pending11;      // an event query issued and not seen done (a late frame's)
bool g_frameLate;                  // this frame's failure is a device that did not finish in time
uint64_t g_lateFrames, g_lateRuns; // frames that went without the passes so, and how many times it began (gfx.state)
uint64_t g_lateRun;                // the frames of the run now (0: the last frame was not late)
double g_lateSince;                // when that run began

IDirect3DVertexShader9 *g_vsQuad;
IDirect3DPixelShader9 *g_psDepth, *g_psComposite;
IDirect3DVertexDeclaration9 *g_decl;
IDirect3DVertexBuffer9 *g_quad;
IDirect3DQuery9 *g_event9;
IDirect3DStateBlock9 *g_block;     // exactly the states the bridge changes

ID3D11Buffer *g_frameCB;
ID3D11SamplerState *g_point, *g_linear;
ID3D11BlendState *g_over;          // premultiplied "over": each pass is laid over what the passes before it drew
ID3D11VertexShader *g_vsFull;
ID3D11Query *g_event11;

// the frame's size
UINT g_w, g_h;
IDirect3DTexture9 *g_intz; IDirect3DSurface9 *g_intzSurf;     // the readable depth buffer
IDirect3DTexture9 *g_color9; IDirect3DSurface9 *g_colorSurf9; // the colour's copy (A8R8G8B8, shared)
IDirect3DTexture9 *g_depth9; IDirect3DSurface9 *g_depthSurf9; // linear depth (R32F, shared)
IDirect3DTexture9 *g_fx9;                                     // the effects target (A16B16G16R16F, shared)
ID3D11Texture2D *g_color11, *g_depth11, *g_fx11;
ID3D11ShaderResourceView *g_colorSRV, *g_depthSRV;
ID3D11RenderTargetView *g_fxRTV;
gfxvk::Link *g_colorLink, *g_depthLink, *g_fxLink;            // on DXVK: each one's two halves (null on 9Ex)

// the sun's shadow map (shadows.cpp makes it shared; opened here while a mod wants it), t2 of every pass
ID3D11Texture2D *g_sun11;
ID3D11ShaderResourceView *g_sunSRV;
gfxvk::Link *g_sunLink;      // on DXVK: the map's Direct3D 11 half, copied each frame
void *g_sunHandle;           // the handle it was opened from (on DXVK: the map's texture)
unsigned g_sunMade, g_sunGen;   // shadows.cpp's count of maps and the device generation it was opened on
std::string g_sunError;

// the 64-bit halves' shared textures (gfx_shared_texture): opened on this device at a pass's first ask, again when the
// half makes one anew (the sidecar's stamp) or the device is new; a failed open is kept, not tried again every frame
struct SideTex {
    MmlMod *mod;
    std::string name;
    uint64_t stamp;
    unsigned gen;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    SidecarTex info;
};
std::vector<SideTex> g_sideTex;
bool g_inPasses;                 // the passes are running (RunPasses)
void ReleaseSideTex(SideTex &t) { SafeRelease(t.srv); SafeRelease(t.tex); }
// the 64-bit halves' shared fences (gfx_shared_fence), opened as the textures are
struct SideFence {
    MmlMod *mod;
    std::string name;
    uint64_t stamp;
    unsigned gen;
    ID3D11Fence *fence;
    uint32_t generation;
};
std::vector<SideFence> g_sideFences;
// the 2D pass for mods (gfx_ui): the HUD and the menus drawn into a shared target (premultiplied RGBA8)
std::vector<std::pair<MmlMod *, int>> g_uiAsk;   // each mod's mode
IDirect3DTexture9 *g_ui9; IDirect3DSurface9 *g_uiSurf9;
ID3D11Texture2D *g_ui11;
ID3D11ShaderResourceView *g_uiSRV;
gfxvk::Link *g_uiLink;
// the motion vectors (motion.cpp; made while a mod wants them): the moving objects' target (A16B16G16R16F: motion,
// view depth, 1), the frame's vectors (G16R16F, shared), the camera the frame before was drawn with
IDirect3DTexture9 *g_motionObj9; IDirect3DSurface9 *g_motionObjSurf9;
IDirect3DTexture9 *g_motion9; IDirect3DSurface9 *g_motionSurf9;
ID3D11Texture2D *g_motion11;
ID3D11ShaderResourceView *g_motionSRV;
gfxvk::Link *g_motionLink;
IDirect3DPixelShader9 *g_psMotionAll;
bool g_motionThisFrame;            // MotionFrameBegin had a camera before: the objects drawn again this frame
bool g_cameraBefore;               // g_viewBefore, g_projBefore hold the last stage 1's camera
float g_viewBefore[16], g_projBefore[16];
unsigned g_motionFrames;
std::string g_motionError;
bool g_motionTried;
bool g_motionReady;                // this frame's vectors drawn and handed over: the passes get them
unsigned g_uiFrames;
IDirect3DSurface9 *g_uiGameRT;     // the frame's target while the 2D pass draws into ours
D3DVIEWPORT9 g_uiViewport;
DWORD g_uiSep, g_uiSrcA, g_uiDstA, g_uiOpA;
int g_uiMode;                      // this 2D pass's

// scene views (view.cpp draws them into these): colour (A8R8G8B8, shared), its readable depth buffer (INTZ), linear
// depth (R32F, shared), at the view's size
struct ViewTargets {
    int handle = 0;
    UINT w = 0, h = 0;
    IDirect3DTexture9 *color9 = nullptr, *intz = nullptr, *depth9 = nullptr;
    IDirect3DSurface9 *colorSurf = nullptr, *intzSurf = nullptr, *depthSurf = nullptr;
    ID3D11Texture2D *color11 = nullptr, *depth11 = nullptr;
    ID3D11ShaderResourceView *colorSRV = nullptr, *depthSRV = nullptr;
    IDirect3DTexture9 *fx9 = nullptr;          // its effects target (gfx_pass_views; made when a pass asks)
    ID3D11Texture2D *fx11 = nullptr;
    ID3D11RenderTargetView *fxRTV = nullptr;
    gfxvk::Link *colorLink = nullptr, *depthLink = nullptr, *fxLink = nullptr;   // on DXVK
    unsigned passRuns = 0, passFails = 0;      // times the passes ran over it, failed
    unsigned generation = 0;       // the device they were made on
    MmlGfxView info{};
    bool depthDone = false;
    float show[4] = {};            // view.show: where it is copied onto the frame (800x600 units; w 0: not shown)
};
std::vector<std::unique_ptr<ViewTargets>> g_views;
ViewTargets *g_viewNow;            // the view being drawn (GfxViewBegin .. GfxViewEnd)
IDirect3DSurface9 *g_viewGameRT, *g_viewGameDS;
D3DVIEWPORT9 g_viewGameVp;
DWORD g_viewClipOn;
float g_viewClip[4];
// A view's clip plane is in world space, as the fixed-function pipeline takes it; LS3D draws its backdrop (the sky)
// with the camera's view without its position (cam + 0x264), so the sky's vertices lie about the world's origin and
// the plane would cut them as if they were there. The sky is infinitely far: while the backdrop's view is set the
// plane is off (SetTransform followed).
bool g_viewClipWanted;
float g_viewBackdrop[16];
unsigned g_viewSkySets;            // the backdrop's view set while a clipped view drew (gfx.state)

// The world's sub-pixel jitter (gfx_jitter): while the frame's world is drawn (GfxBeforeWorld .. GfxAfterWorld) every
// perspective projection the game sets goes to the device offset by the mod's pixels, the scene views' and the sun's
// map not. What the bridge gives (the frame's view and proj, the constants) is without it; the motion vectors take the
// last frame's camera through this frame's jitter, so it cancels in them while their pixels are the drawn picture's.
struct JitterAsk { MmlMod *mod = nullptr; float want[2] = {0, 0}; } g_jitter;
float g_jitNow[2];                 // this frame's world's (pixels; 0 0: none)
bool g_jitWorld;                   // the frame's world is being drawn with it
D3DMATRIX g_projClean;             // the last perspective projection the game set, as it set it
bool g_projCleanHave;

// the frame in flight
IDirect3DSurface9 *g_gameDS;       // the game's depth buffer while ours is bound
bool g_swapped;
std::string g_frameError;
MmlGfxConstants g_k;
float g_viewM[16], g_projM[16];

// The upscaler (gfx_upscale): the world drawn at its render size, its function first at stage 1.
struct UpscaleAsk { MmlMod *mod = nullptr; MmlGfxUpscaleFn fn = nullptr; void *user = nullptr; UINT w = 0, h = 0; } g_up;
UINT g_dw, g_dh;                   // the size this frame's world is drawn at (the frame's, or the upscaler's)
bool g_upFrame;                    // this frame's world is the upscaler's (its function runs at stage 1)
bool g_scaledNow;                  // the world is being drawn smaller now (GfxBeforeWorld .. stage 1): viewports scaled
bool g_rtFrame;                    // the device's target 0 is the frame's size (a viewport on it is scaled)
D3DVIEWPORT9 g_vpGame;             // the last viewport the game set, as it set it
unsigned g_upDrawn, g_upStretched; // frames the upscaler drew, frames the bridge stretched (gfx.state)
double g_upCpuMs, g_upGpuMs;       // smoothed: the upscaler's function's time, its GPU time
// the frame at its size for the passes after the upscaler: its picture, the depth and the motion brought up to it
ID3D11Texture2D *g_upColor11, *g_upDepth11, *g_upMotion11;
ID3D11ShaderResourceView *g_upColorSRV, *g_upDepthSRV, *g_upMotionSRV;
ID3D11RenderTargetView *g_upDepthRTV, *g_upMotionRTV;
ID3D11PixelShader *g_psStretch, *g_psUpDepth, *g_psUpMotion;
ID3D11Buffer *g_upCB;              // bridge11.hlsl's Up (b1): the render size, the frame's
bool g_upMade;                     // this frame's g_up* written (the showing gives them too)

// statistics
LARGE_INTEGER g_freq;
unsigned g_frames, g_notOurDepth, g_skipped;
double g_ms9, g_ms11, g_msAll, g_wait11;
double NowMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return 1000.0 * t.QuadPart / g_freq.QuadPart; }

// The frame's times, summed over the frames since they were last logged (once a minute) or reset.
struct Timing {
    uint64_t frames = 0, gpuFrames = 0, passFrames = 0;
    double frameMs = 0, presentMs = 0, wait9Ms = 0, wait11Ms = 0, handMs = 0, gpuMs = 0, passesMs = 0;
};
Timing g_tm, g_tmLogged;
double g_tmLoggedAt;
// Direct3D 9's GPU time of a frame: timestamps at its first drawing and at its Present, read a few frames later
struct Stamp { IDirect3DQuery9 *disjoint = nullptr, *freq = nullptr, *begin = nullptr, *end = nullptr; bool open = false, pending = false; };
constexpr int kStamps = 6;
Stamp g_stamps[kStamps];
int g_stampNext;
Stamp *g_stampNow;
double g_gpuFrame;                 // the last read frame's, ms

std::string Fmt(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

// ---- the world's fog and sun, as the game draws with them (see the top)
using SetRenderStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
using SetLightFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, const D3DLIGHT9 *);
using LightEnableFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, BOOL);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *,
                                                             D3DFORMAT, const void *, UINT);
using SetTransformFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
using SetDepthStencilFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
using SetViewportFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
// IDirect3DDevice9's vtable slots (d3d9.h's order)
constexpr int kVtSetRenderTarget = 37, kVtSetDepthStencil = 39, kVtSetTransform = 44, kVtSetViewport = 47, kVtSetLight = 51,
              kVtLightEnable = 53, kVtSetRenderState = 57, kVtSetSamplerState = 69, kVtDrawPrimitive = 81, kVtDrawIndexedPrimitive = 82,
              kVtDrawPrimitiveUP = 83, kVtDrawIndexedPrimitiveUP = 84;
SetRenderTargetFn oSetRenderTarget;
SetViewportFn oSetViewport;
SetSamplerStateFn oSetSamplerState;
SetDepthStencilFn oSetDepthStencil;
SetTransformFn oSetTransform;
SetRenderStateFn oSetRenderState;
SetLightFn oSetLight;
LightEnableFn oLightEnable;
DrawPrimitiveFn oDrawPrimitive;
DrawIndexedPrimitiveFn oDrawIndexedPrimitive;
DrawPrimitiveUPFn oDrawPrimitiveUP;
DrawIndexedPrimitiveUPFn oDrawIndexedPrimitiveUP;
bool g_stateHooked;
int g_quiet;                       // the bridge's own calls and the mods' drawing: not the game's

struct Fog { DWORD on = 0, color = 0, table = 0, vertex = 0, start = 0, end = 0, density = 0; };
bool operator==(const Fog &a, const Fog &b) {
    return a.on == b.on && a.color == b.color && a.table == b.table && a.vertex == b.vertex && a.start == b.start &&
           a.end == b.end && a.density == b.density;
}
Fog g_fogNow;
DWORD g_lighting = TRUE;
struct Light8 { D3DLIGHT9 light{}; bool set = false, on = false; };
Light8 g_lights[8];
struct FogTally { Fog fog; double prims = 0; };
struct SunTally { D3DVECTOR dir{}; D3DCOLORVALUE color{}; double prims = 0; };
FogTally g_fogTally[6];
SunTally g_sunTally[6];
int g_fogTallies, g_sunTallies;
double g_framePrims, g_litPrims;
Fog g_worldFog;
SunTally g_worldSun;
bool g_haveWorldFog, g_haveWorldSun;
DWORD g_sceneSunAt;
bool g_haveSceneSun;
float g_sceneSun[3], g_sceneSunColor[3], g_sceneAmbient[3];
const char *g_sunFrom = "a default from above";

void Tally(UINT prims) {
    if (g_quiet || !prims) return;
    g_framePrims += prims;
    if (g_lighting) g_litPrims += prims;
    if (g_fogNow.on) {
        int i = 0;
        while (i < g_fogTallies && !(g_fogTally[i].fog == g_fogNow)) i++;
        if (i == g_fogTallies && g_fogTallies < 6) g_fogTally[g_fogTallies++] = {g_fogNow, 0};
        if (i < g_fogTallies) g_fogTally[i].prims += prims;
    }
    if (!g_lighting) return;
    const D3DLIGHT9 *best = nullptr;   // the brightest enabled directional light this draw is lit with
    float bestLum = 0;
    for (const Light8 &s : g_lights) {
        if (!s.set || !s.on || s.light.Type != D3DLIGHT_DIRECTIONAL) continue;
        float lum = 0.3f * s.light.Diffuse.r + 0.59f * s.light.Diffuse.g + 0.11f * s.light.Diffuse.b;
        if (lum > bestLum) { bestLum = lum; best = &s.light; }
    }
    if (!best) return;
    int i = 0;
    auto same = [&](const SunTally &t) {
        return t.dir.x == best->Direction.x && t.dir.y == best->Direction.y && t.dir.z == best->Direction.z &&
               t.color.r == best->Diffuse.r && t.color.g == best->Diffuse.g && t.color.b == best->Diffuse.b;
    };
    while (i < g_sunTallies && !same(g_sunTally[i])) i++;
    if (i == g_sunTallies && g_sunTallies < 6) g_sunTally[g_sunTallies++] = {best->Direction, best->Diffuse, 0};
    if (i < g_sunTallies) g_sunTally[i].prims += prims;
}

// The readable depth buffer stands for the game's while the world is drawn (GfxBeforeWorld .. GfxAfterWorld): a set of
// the game's own depth buffer then binds the bridge's, and out of that time a set of the bridge's binds the game's.
// Another mod may keep the depth buffer it found at its start and put it back after its own drawing: a player's water
// shader (MafiaWaterShaderMod.asi) did so around every water draw, through Direct3D 8's SetRenderTarget, and the world
// was drawn into the game's depth buffer, which nobody had cleared this frame: the sky alone was seen (2026-10-22).
IDirect3DSurface9 *g_lastGameDS;   // the game's depth buffer at the last frame's swap (compared only, not held)
unsigned g_depthRedirects;
HRESULT STDMETHODCALLTYPE hkSetDepthStencil(IDirect3DDevice9 *d, IDirect3DSurface9 *s) {
    if (s && d == g_dev9) {
        if (g_swapped && s == g_gameDS && g_intzSurf) { s = g_intzSurf; g_depthRedirects++; }
        else if (!g_swapped && s == g_intzSurf && g_lastGameDS) { s = g_lastGameDS; g_depthRedirects++; }
    }
    return oSetDepthStencil(d, s);
}

HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    HRESULT hr = oSetRenderState(d, s, v);
    if (g_quiet || FAILED(hr)) return hr;
    switch (s) {
    case D3DRS_FOGENABLE: g_fogNow.on = v; break;
    case D3DRS_FOGCOLOR: g_fogNow.color = v; break;
    case D3DRS_FOGTABLEMODE: g_fogNow.table = v; break;
    case D3DRS_FOGVERTEXMODE: g_fogNow.vertex = v; break;
    case D3DRS_FOGSTART: g_fogNow.start = v; break;
    case D3DRS_FOGEND: g_fogNow.end = v; break;
    case D3DRS_FOGDENSITY: g_fogNow.density = v; break;
    case D3DRS_LIGHTING: g_lighting = v; break;
    default: break;
    }
    return hr;
}
// A perspective projection (row vectors: w is the view depth times _34): (x, y) pixels of the picture as drawn (the
// frame's, or the upscaler's render size) added to where it puts every point, times sign (-1 takes them back)
bool Perspective(const D3DMATRIX &m) { return m._34 != 0.0f && m._44 == 0.0f; }
void JitterInto(D3DMATRIX &m, const float j[2], float sign) {
    const UINT w = g_dw ? g_dw : g_w, h = g_dh ? g_dh : g_h;
    if (!Perspective(m) || !w || !h) return;
    m._31 += sign * 2.0f * j[0] / static_cast<float>(w) * m._34;
    m._32 -= sign * 2.0f * j[1] / static_cast<float>(h) * m._34;
}

// The world drawn smaller (the upscaler's render size): a viewport on the frame's target scaled from the frame's size
// to it, rounded to whole pixels at both edges (the game's full-frame viewport becomes 0, 0, the render size).
D3DVIEWPORT9 Scaled(const D3DVIEWPORT9 &v) {
    D3DVIEWPORT9 s = v;
    if (!g_w || !g_h) return s;
    auto at = [](DWORD x, UINT to, UINT from) { return static_cast<DWORD>((static_cast<uint64_t>(x) * to + from / 2) / from); };
    s.X = at(v.X, g_dw, g_w), s.Y = at(v.Y, g_dh, g_h);
    s.Width = std::max<DWORD>(1, at(v.X + v.Width, g_dw, g_w) - s.X);
    s.Height = std::max<DWORD>(1, at(v.Y + v.Height, g_dh, g_h) - s.Y);
    return s;
}
bool IsFrameSize(IDirect3DSurface9 *s) {
    D3DSURFACE_DESC d{};
    return s && SUCCEEDED(s->GetDesc(&d)) && d.Width == g_w && d.Height == g_h;
}
HRESULT STDMETHODCALLTYPE hkSetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    if (d != g_dev9 || g_quiet || !v) return oSetViewport(d, v);
    if (g_rtFrame) g_vpGame = *v;
    if (!g_scaledNow || !g_rtFrame || ViewDrawingNow()) return oSetViewport(d, v);
    D3DVIEWPORT9 s = Scaled(*v);
    return oSetViewport(d, &s);
}
// Setting a target sets the viewport to the whole of it (Direct3D 9, as 8): the frame's target set again while the
// world is drawn smaller takes the scaled one (LS3D sets its viewport only when its rectangle changes).
// The textures' mip bias while the world is drawn smaller (DLSS's guide: log2(render width / display width) added to the
// renderer's own): a texture's mip is chosen for the pixels it covers, and at the render size those are fewer, so
// without it every texture is the render size's, blurred (a road at a grazing angle most), and DLSS cannot bring back
// what was never sampled. The game's own bias (LS3D sets none) is followed per sampler and the scaled one added.
constexpr DWORD kSamplers = 16;
float g_lodGame[kSamplers];        // the game's own D3DSAMP_MIPMAPLODBIAS per sampler
float g_lodBias;                   // added while the world is drawn smaller (0 otherwise)
float g_lodAsked = NAN;            // gfx.state {"mip_bias"}: another bias in its place (a test; NaN: DLSS's own)
DWORD Bits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }
float Float(DWORD d) { float f; memcpy(&f, &d, 4); return f; }
HRESULT STDMETHODCALLTYPE hkSetSamplerState(IDirect3DDevice9 *d, DWORD i, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (d != g_dev9 || g_quiet || t != D3DSAMP_MIPMAPLODBIAS || i >= kSamplers) return oSetSamplerState(d, i, t, v);
    g_lodGame[i] = Float(v);
    return oSetSamplerState(d, i, t, g_lodBias != 0 && !ViewDrawingNow() ? Bits(g_lodGame[i] + g_lodBias) : v);
}
void LodBias(float bias) {
    if (bias == g_lodBias || !g_dev9 || !oSetSamplerState) return;
    g_lodBias = bias;
    for (DWORD i = 0; i < kSamplers; i++) oSetSamplerState(g_dev9, i, D3DSAMP_MIPMAPLODBIAS, Bits(g_lodGame[i] + bias));
}

HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s) {
    HRESULT hr = oSetRenderTarget(d, i, s);
    if (d != g_dev9 || i != 0 || FAILED(hr)) return hr;
    g_rtFrame = IsFrameSize(s);
    if (g_quiet || !g_rtFrame) return hr;
    g_vpGame = D3DVIEWPORT9{0, 0, g_w, g_h, 0, 1};
    if (g_scaledNow && !ViewDrawingNow()) {
        D3DVIEWPORT9 v = Scaled(g_vpGame);
        oSetViewport(d, &v);
    }
    return hr;
}
void JitterInto(float *m16, const float j[2], float sign) {
    D3DMATRIX m;
    memcpy(&m, m16, sizeof m);
    JitterInto(m, j, sign);
    memcpy(m16, &m, sizeof m);
}

HRESULT STDMETHODCALLTYPE hkSetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m) {
    D3DMATRIX jittered;
    if (s == D3DTS_PROJECTION && m && !g_quiet && Perspective(*m)) {
        g_projClean = *m, g_projCleanHave = true;
        if (g_jitWorld && !ViewDrawingNow()) {
            jittered = *m;
            JitterInto(jittered, g_jitNow, 1);
            m = &jittered;
        }
    }
    HRESULT hr = oSetTransform(d, s, m);
    if (g_viewClipWanted && !g_quiet && s == D3DTS_VIEW && m && SUCCEEDED(hr)) {   // (see g_viewClipWanted)
        bool sky = !memcmp(m, g_viewBackdrop, sizeof g_viewBackdrop);
        if (sky) g_viewSkySets++;
        oSetRenderState(d, D3DRS_CLIPPLANEENABLE, sky ? 0 : D3DCLIPPLANE0);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkSetLight(IDirect3DDevice9 *d, DWORD i, const D3DLIGHT9 *l) {
    HRESULT hr = oSetLight(d, i, l);
    if (!g_quiet && SUCCEEDED(hr) && l && i < 8) { g_lights[i].light = *l; g_lights[i].set = true; }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkLightEnable(IDirect3DDevice9 *d, DWORD i, BOOL on) {
    HRESULT hr = oLightEnable(d, i, on);
    if (!g_quiet && SUCCEEDED(hr) && i < 8) g_lights[i].on = on != FALSE;
    return hr;
}
HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT start, UINT n) {
    Tally(n);
    return oDrawPrimitive(d, t, start, n);
}
HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts,
                                                 UINT start, UINT n) {
    Tally(n);
    return oDrawIndexedPrimitive(d, t, base, minIndex, verts, start, n);
}
HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT n, const void *data, UINT stride) {
    Tally(n);
    return oDrawPrimitiveUP(d, t, n, data, stride);
}
HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT minIndex, UINT verts, UINT n,
                                                   const void *idx, D3DFORMAT fmt, const void *data, UINT stride) {
    Tally(n);
    return oDrawIndexedPrimitiveUP(d, t, minIndex, verts, n, idx, fmt, data, stride);
}

void ReadStateNow(IDirect3DDevice9 *d) {   // the state as the game set it before the hooks went in
    for (DWORD i = 0; i < kSamplers; i++) {
        DWORD v = 0;
        g_lodGame[i] = SUCCEEDED(d->GetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, &v)) ? Float(v) : 0;
    }
    d->GetRenderState(D3DRS_FOGENABLE, &g_fogNow.on);
    d->GetRenderState(D3DRS_FOGCOLOR, &g_fogNow.color);
    d->GetRenderState(D3DRS_FOGTABLEMODE, &g_fogNow.table);
    d->GetRenderState(D3DRS_FOGVERTEXMODE, &g_fogNow.vertex);
    d->GetRenderState(D3DRS_FOGSTART, &g_fogNow.start);
    d->GetRenderState(D3DRS_FOGEND, &g_fogNow.end);
    d->GetRenderState(D3DRS_FOGDENSITY, &g_fogNow.density);
    d->GetRenderState(D3DRS_LIGHTING, &g_lighting);
    for (DWORD i = 0; i < 8; i++) {
        BOOL on = FALSE;
        g_lights[i].set = SUCCEEDED(d->GetLight(i, &g_lights[i].light));
        g_lights[i].on = SUCCEEDED(d->GetLightEnable(i, &on)) && on;
    }
}

void HookState(IDirect3DDevice9 *d) {
    if (g_stateHooked || !d) return;
    void **vt = *reinterpret_cast<void ***>(d);
    struct { int slot; void *hook; void **orig; } hooks[] = {
        {kVtSetRenderState, reinterpret_cast<void *>(&hkSetRenderState), reinterpret_cast<void **>(&oSetRenderState)},
        {kVtSetDepthStencil, reinterpret_cast<void *>(&hkSetDepthStencil), reinterpret_cast<void **>(&oSetDepthStencil)},
        {kVtSetRenderTarget, reinterpret_cast<void *>(&hkSetRenderTarget), reinterpret_cast<void **>(&oSetRenderTarget)},
        {kVtSetViewport, reinterpret_cast<void *>(&hkSetViewport), reinterpret_cast<void **>(&oSetViewport)},
        {kVtSetSamplerState, reinterpret_cast<void *>(&hkSetSamplerState), reinterpret_cast<void **>(&oSetSamplerState)},
        {kVtSetTransform, reinterpret_cast<void *>(&hkSetTransform), reinterpret_cast<void **>(&oSetTransform)},
        {kVtSetLight, reinterpret_cast<void *>(&hkSetLight), reinterpret_cast<void **>(&oSetLight)},
        {kVtLightEnable, reinterpret_cast<void *>(&hkLightEnable), reinterpret_cast<void **>(&oLightEnable)},
        {kVtDrawPrimitive, reinterpret_cast<void *>(&hkDrawPrimitive), reinterpret_cast<void **>(&oDrawPrimitive)},
        {kVtDrawIndexedPrimitive, reinterpret_cast<void *>(&hkDrawIndexedPrimitive), reinterpret_cast<void **>(&oDrawIndexedPrimitive)},
        {kVtDrawPrimitiveUP, reinterpret_cast<void *>(&hkDrawPrimitiveUP), reinterpret_cast<void **>(&oDrawPrimitiveUP)},
        {kVtDrawIndexedPrimitiveUP, reinterpret_cast<void *>(&hkDrawIndexedPrimitiveUP), reinterpret_cast<void **>(&oDrawIndexedPrimitiveUP)},
    };
    ReadStateNow(d);
    std::string err;
    for (auto &h : hooks) {
        if (HookAdd(nullptr, reinterpret_cast<uintptr_t>(vt[h.slot]), h.hook, h.orig, &err)) continue;
        for (auto &u : hooks) if (*u.orig) HookRemoveLink(nullptr, reinterpret_cast<uintptr_t>(vt[u.slot]), u.hook);
        HWARN("gfx: the device's state calls not followed (%s): passes get no fog and a default sun", err.c_str());
        return;
    }
    g_stateHooked = true;
}

// The frame up to stage 1 is over: its most drawn-with fog and sun become the world's.
void TakeWorldLight() {
    int f = -1, s = -1;
    for (int i = 0; i < g_fogTallies; i++) if (f < 0 || g_fogTally[i].prims > g_fogTally[f].prims) f = i;
    for (int i = 0; i < g_sunTallies; i++) if (s < 0 || g_sunTally[i].prims > g_sunTally[s].prims) s = i;
    if (f >= 0) { g_worldFog = g_fogTally[f].fog; g_haveWorldFog = true; }
    else if (g_framePrims > 0) { g_worldFog = Fog{}; g_haveWorldFog = true; }
    if (s >= 0) g_worldSun = g_sunTally[s];
    if (s >= 0 || g_litPrims > 0) g_haveWorldSun = s >= 0;
    g_fogTallies = g_sunTallies = 0;
    g_framePrims = g_litPrims = 0;
}

// ---- GPU time of each pass: timestamps around it in a disjoint query, read back a few frames later
constexpr int kTimerFrames = 4, kTimerMarks = 66;   // a frame: the start, after each pass (64 passes at most)
struct TimerFrame { ID3D11Query *disjoint = nullptr, *ts[kTimerMarks] = {}; int marks = 0; bool pending = false; std::vector<int> handles; };
TimerFrame g_timers[kTimerFrames];
int g_timerNext;
double g_gpuPasses;   // the last read frame's passes, whole, ms

void ReleaseTimers() {
    for (TimerFrame &t : g_timers) {
        SafeRelease(t.disjoint);
        for (ID3D11Query *&q : t.ts) SafeRelease(q);
        t.marks = 0;
        t.pending = false;
    }
}
void ReadTimers() {
    for (TimerFrame &t : g_timers) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (g_ctx->GetData(t.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        t.pending = false;
        if (dj.Disjoint || !dj.Frequency) continue;
        std::vector<UINT64> v(t.marks);
        bool all = true;
        for (int i = 0; i < t.marks && all; i++)
            all = g_ctx->GetData(t.ts[i], &v[i], sizeof v[i], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!all) continue;
        auto ms = [&](int a, int b) { return 1000.0 * static_cast<double>(v[b] - v[a]) / static_cast<double>(dj.Frequency); };
        g_gpuPasses = t.marks > 1 ? ms(0, t.marks - 1) : 0;
        g_tm.passesMs += g_gpuPasses, g_tm.passFrames++;
        for (int i = 1; i < t.marks; i++) {
            int h = t.handles[i - 1];
            if (h == -1) g_upGpuMs = g_upGpuMs <= 0 ? ms(i - 1, i) : g_upGpuMs * 0.9 + ms(i - 1, i) * 0.1;   // (the upscaler's)
            for (Pass &p : g_passes)
                if (p.handle == h) p.gpuMs = p.gpuMs <= 0 ? ms(i - 1, i) : p.gpuMs * 0.9 + ms(i - 1, i) * 0.1;
        }
    }
}
TimerFrame *BeginTimers() {
    TimerFrame &t = g_timers[g_timerNext];
    if (t.pending) return nullptr;   // the GPU is that far behind: this frame goes untimed
    if (!t.disjoint) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(g_dev11->CreateQuery(&qd, &t.disjoint))) return nullptr;
    }
    g_timerNext = (g_timerNext + 1) % kTimerFrames;
    t.marks = 0;
    t.handles.clear();
    g_ctx->Begin(t.disjoint);
    return &t;
}
void Mark(TimerFrame *t, int handle) {
    if (!t || t->marks >= kTimerMarks) return;
    ID3D11Query *&q = t->ts[t->marks];
    if (!q) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(g_dev11->CreateQuery(&qd, &q))) return;
    }
    g_ctx->End(q);
    if (t->marks > 0) t->handles.push_back(handle);
    t->marks++;
}
void EndTimers(TimerFrame *t) {
    if (!t) return;
    g_ctx->End(t->disjoint);
    t->pending = true;
}

// ---- set up and tear down
void ReleaseView(ViewTargets &v) {
    gfxvk::Free(v.colorLink); gfxvk::Free(v.depthLink); gfxvk::Free(v.fxLink);
    SafeRelease(v.colorSRV); SafeRelease(v.depthSRV); SafeRelease(v.color11); SafeRelease(v.depth11);
    SafeRelease(v.colorSurf); SafeRelease(v.color9); SafeRelease(v.intzSurf); SafeRelease(v.intz);
    SafeRelease(v.depthSurf); SafeRelease(v.depth9);
    SafeRelease(v.fxRTV); SafeRelease(v.fx11); SafeRelease(v.fx9);
    v.w = v.h = 0;
    v.info.frames = 0;
    v.info.fresh = 0;
}

void ReleaseSized() {
    gfxvk::Free(g_colorLink); gfxvk::Free(g_depthLink); gfxvk::Free(g_fxLink); gfxvk::Free(g_uiLink); gfxvk::Free(g_sunLink);
    gfxvk::Free(g_motionLink);
    SafeRelease(g_motionSRV); SafeRelease(g_motion11); SafeRelease(g_motionSurf9); SafeRelease(g_motion9);
    SafeRelease(g_motionObjSurf9); SafeRelease(g_motionObj9);
    g_cameraBefore = false;
    g_motionTried = false;
    g_motionReady = false;
    SafeRelease(g_colorSRV); SafeRelease(g_depthSRV); SafeRelease(g_fxRTV);
    SafeRelease(g_color11); SafeRelease(g_depth11); SafeRelease(g_fx11);
    SafeRelease(g_intzSurf); SafeRelease(g_intz);
    SafeRelease(g_colorSurf9); SafeRelease(g_color9);
    SafeRelease(g_depthSurf9); SafeRelease(g_depth9);
    SafeRelease(g_fx9);
    SafeRelease(g_uiSRV); SafeRelease(g_ui11); SafeRelease(g_uiSurf9); SafeRelease(g_ui9);
    SafeRelease(g_upColorSRV); SafeRelease(g_upDepthSRV); SafeRelease(g_upMotionSRV);
    SafeRelease(g_upDepthRTV); SafeRelease(g_upMotionRTV);
    SafeRelease(g_upColor11); SafeRelease(g_upDepth11); SafeRelease(g_upMotion11);
    g_upMade = false;
    g_uiFrames = 0;
    g_w = g_h = 0;
    g_dw = g_dh = 0;
    SafeRelease(g_sunSRV); SafeRelease(g_sun11);
    g_sunHandle = nullptr;
}

int UiMode() {
    int m = MML_GFX_UI_OFF;
    for (auto &a : g_uiAsk) if (ModAlive(a.first)) m = std::max(m, a.second);
    return m;
}

void ReleaseShow();   // (the showing's textures and thread: below)
void ShowThreadStop();
void ReleaseAll() {
    bool wasSwapped = g_swapped;
    g_swapped = false;   // (first: hkSetDepthStencil binds the game's own then)
    if (wasSwapped && g_dev9 && g_gameDS) g_dev9->SetDepthStencilSurface(g_gameDS);
    SafeRelease(g_gameDS);
    g_lastGameDS = nullptr;
    g_pending9 = g_pending11 = false;
    g_lateRun = 0;
    ReleaseShow();
    ReleaseSized();
    for (auto &v : g_views) ReleaseView(*v);
    ReleaseTimers();
    for (auto &t : g_sideTex) ReleaseSideTex(t);
    g_sideTex.clear();
    for (auto &f : g_sideFences) SafeRelease(f.fence);
    g_sideFences.clear();
    SafeRelease(g_vsQuad); SafeRelease(g_psDepth); SafeRelease(g_psComposite);
    SafeRelease(g_decl); SafeRelease(g_quad); SafeRelease(g_event9); SafeRelease(g_block); SafeRelease(g_psMotionAll);
    MotionDeviceGone();
    SafeRelease(g_frameCB); SafeRelease(g_point); SafeRelease(g_linear); SafeRelease(g_vsFull); SafeRelease(g_over);
    SafeRelease(g_event11);
    SafeRelease(g_psStretch); SafeRelease(g_psUpDepth); SafeRelease(g_psUpMotion); SafeRelease(g_upCB);
    for (Stamp &s : g_stamps) { SafeRelease(s.disjoint); SafeRelease(s.freq); SafeRelease(s.begin); SafeRelease(s.end); s = Stamp{}; }
    g_stampNow = nullptr;
    g_scaledNow = false;
    g_lodBias = 0;
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); }
    gfxvk::Stop();   // (every link freed above)
    SafeRelease(g_ctx); SafeRelease(g_dev11);
    SafeRelease(g_dev9);
    g_d3d8 = nullptr;
}

bool Fail(const std::string &why) {
    g_state = "off: " + why;
    HWARN("gfx: %s", g_state.c_str());
    g_failed = true;
    ReleaseAll();
    return false;
}

// Windows' own dxgi.dll and d3d11.dll, by their full paths in System32, dxgi first (d3d11's import of it then binds to
// that one): a player's game folder may hold DXVK's dxgi.dll (or d3d11.dll), which a bare name would find first, and
// Windows' d3d11.dll given DXVK's adapter faulted (seen 2026-10-19). Loaded before the mods (hkDirect3DCreate8, when a
// mod wants the bridge), so the mods' own imports of them by name find these, already loaded.
HMODULE SystemDll(const wchar_t *name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    std::wstring path = std::wstring(dir) + L"\\" + name;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
}
bool LoadD3d11() {
    if (pD3D11CreateDevice && pCreateDXGIFactory1) return true;
    HMODULE dx = SystemDll(L"dxgi.dll"), d11 = SystemDll(L"d3d11.dll");
    pD3D11CreateDevice = d11 ? reinterpret_cast<CreateDevice11Fn>(GetProcAddress(d11, "D3D11CreateDevice")) : nullptr;
    pCreateDXGIFactory1 = dx ? reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dx, "CreateDXGIFactory1")) : nullptr;
    return pD3D11CreateDevice && pCreateDXGIFactory1;
}

// The Direct3D 11 device on the adapter the game renders with (matched by its LUID).
bool MakeDevice11() {
    if (!LoadD3d11()) return Fail("d3d11.dll or dxgi.dll not found");
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(g_dev9->GetCreationParameters(&cp))) return Fail("GetCreationParameters failed");
    IDirect3D9 *d3d9 = nullptr;
    if (FAILED(g_dev9->GetDirect3D(&d3d9)) || !d3d9) return Fail("GetDirect3D failed");
    IDirect3D9Ex *d3d9ex = nullptr;
    LUID luid{};
    bool haveLuid = SUCCEEDED(d3d9->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void **>(&d3d9ex))) && d3d9ex &&
                    SUCCEEDED(d3d9ex->GetAdapterLUID(cp.AdapterOrdinal, &luid));
    bool intz = SUCCEEDED(d3d9->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                                  D3DRTYPE_TEXTURE, kFmtIntz));
    SafeRelease(d3d9ex);
    SafeRelease(d3d9);
    if (!haveLuid) haveLuid = gfxvk::Luid(&luid);   // (DXVK's plain Direct3D 9: its Vulkan device's)
    if (haveLuid) SidecarAdapter(luid.LowPart, static_cast<uint32_t>(luid.HighPart));
    if (!haveLuid) return Fail("no adapter LUID from Direct3D 9Ex nor DXVK's Vulkan device");
    if (!intz) return Fail("the adapter has no INTZ (readable depth) format");
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(pCreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) return Fail("CreateDXGIFactory1 failed");
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(adapter->GetDesc1(&d)) && d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
            g_adapter = name;
            break;
        }
        SafeRelease(adapter);
    }
    SafeRelease(factory);
    if (!adapter) return Fail("no DXGI adapter with the game's LUID");
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = pD3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_dev11, &got, &g_ctx);
    if (hr == E_INVALIDARG)   // a runtime without 11.1
        hr = pD3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &g_dev11, &got, &g_ctx);
    SafeRelease(adapter);
    if (FAILED(hr)) return Fail(Fmt("D3D11CreateDevice failed (0x%08X)", static_cast<unsigned>(hr)));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(MmlGfxConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev11->CreateBuffer(&bd, nullptr, &g_frameCB))) return Fail("the frame's constant buffer");
    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(g_dev11->CreateSamplerState(&sd, &g_point))) return Fail("the point sampler");
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(g_dev11->CreateSamplerState(&sd, &g_linear))) return Fail("the linear sampler");
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_dev11->CreateBlendState(&bl, &g_over))) return Fail("the passes' blend state");
    if (FAILED(g_dev11->CreateVertexShader(kVsFullScreen, sizeof kVsFullScreen, nullptr, &g_vsFull))) return Fail("the full-screen vertex shader");
    if (FAILED(g_dev11->CreatePixelShader(kPsStretch, sizeof kPsStretch, nullptr, &g_psStretch)) ||
        FAILED(g_dev11->CreatePixelShader(kPsUpDepth, sizeof kPsUpDepth, nullptr, &g_psUpDepth)) ||
        FAILED(g_dev11->CreatePixelShader(kPsUpMotion, sizeof kPsUpMotion, nullptr, &g_psUpMotion)))
        return Fail("the upscaler's shaders");
    D3D11_BUFFER_DESC ub{};
    ub.ByteWidth = 16;
    ub.Usage = D3D11_USAGE_DEFAULT;
    ub.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev11->CreateBuffer(&ub, nullptr, &g_upCB))) return Fail("the upscaler's constant buffer");
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    if (FAILED(g_dev11->CreateQuery(&qd, &g_event11))) return Fail("the Direct3D 11 event query");
    HLOG("gfx: Direct3D 11 device on %s (feature level %x.%x)", g_adapter.c_str(), (got >> 12) & 0xF, (got >> 8) & 0xF);
    return true;
}

bool MakeFixed9() {
    if (FAILED(g_dev9->CreateVertexShader(reinterpret_cast<const DWORD *>(kVsQuad), &g_vsQuad))) return Fail("vs_3_0 quad");
    if (FAILED(g_dev9->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsDepth), &g_psDepth))) return Fail("ps_3_0 depth");
    if (FAILED(g_dev9->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsComposite), &g_psComposite))) return Fail("ps_3_0 composite");
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    if (FAILED(g_dev9->CreateVertexDeclaration(el, &g_decl))) return Fail("the vertex declaration");
    if (FAILED(g_dev9->CreateVertexBuffer(4 * 16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_quad, nullptr))) return Fail("the quad");
    float *v = nullptr;
    if (FAILED(g_quad->Lock(0, 0, reinterpret_cast<void **>(&v), 0))) return Fail("the quad's lock");
    const float q[16] = {-1, 1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, -1, 0, 1};   // a triangle strip, clip space
    memcpy(v, q, sizeof q);
    g_quad->Unlock();
    if (FAILED(g_dev9->CreateQuery(D3DQUERYTYPE_EVENT, &g_event9))) return Fail("the Direct3D 9 event query");
    // The states the bridge changes, recorded into a block (their values do not matter while recording); the
    // shaders, the declaration or FVF and stream 0 are saved by hand (Saved9).
    if (FAILED(g_dev9->BeginStateBlock())) return Fail("BeginStateBlock");
    const D3DRENDERSTATETYPE rs[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
                                     D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
                                     D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE,
                                     D3DRS_SRGBWRITEENABLE, D3DRS_FILLMODE, D3DRS_CLIPPLANEENABLE};
    for (D3DRENDERSTATETYPE s : rs) g_dev9->SetRenderState(s, 0);
    const D3DSAMPLERSTATETYPE ss[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV,
                                      D3DSAMP_SRGBTEXTURE};
    for (D3DSAMPLERSTATETYPE s : ss) g_dev9->SetSamplerState(0, s, 0);
    g_dev9->SetTexture(0, nullptr);
    const float zero[8] = {};
    g_dev9->SetPixelShaderConstantF(0, zero, 2);
    D3DVIEWPORT9 vp{0, 0, 1, 1, 0, 1};
    g_dev9->SetViewport(&vp);
    if (FAILED(g_dev9->EndStateBlock(&g_block)) || !g_block) return Fail("EndStateBlock");
    return true;
}

bool Acquire(void *d3d8) {
    if (g_d3d8 == d3d8 && g_dev9) return true;
    if (g_d3d8 && g_d3d8 != d3d8) { HLOG("gfx: the game's device changed: made anew"); ReleaseAll(); g_failed = false; }
    if (g_failed || !d3d8) return false;
    g_d3d8 = d3d8;
    auto qi = (*reinterpret_cast<HRESULT(__stdcall ***)(void *, REFIID, void **)>(d3d8))[0];
    // Direct3D 9Ex shares textures by handle (dxwrapper with D3d8to9 and D3d9to9Ex); DXVK's Direct3D 9 shares through
    // Vulkan whether it was made extended or not (Ultimate ASI Loader's d3d8to9, Mafia Fusion Fix's, makes a plain one)
    IDirect3DDevice9Ex *ex = nullptr;
    if (SUCCEEDED(qi(d3d8, __uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(&ex))) && ex) {
        g_dev9 = ex;
    } else if (FAILED(qi(d3d8, __uuidof(IDirect3DDevice9), reinterpret_cast<void **>(&g_dev9))) || !g_dev9 || !gfxvk::IsDxvk(g_dev9)) {
        SafeRelease(g_dev9);
        return Fail("the game's device is neither Direct3D 9Ex underneath (dxwrapper with D3d8to9 and D3d9to9Ex) nor DXVK's "
                    "Direct3D 9 (a d3d8to9 over DXVK's d3d9.dll)");
    }
    std::string why;
    if (gfxvk::Start(g_dev9, &why)) HLOG("gfx: Direct3D 9 is DXVK's (Vulkan): the shared textures are copied on its queue");
    else if (!why.empty()) return Fail(why);
    if (!MakeDevice11() || !MakeFixed9()) return false;
    if (gfxvk::On()) {
        if (gfxvk::Fences(g_dev11, g_ctx, &why))
            HLOG("gfx: DXVK and Direct3D 11 wait for each other on the GPU (shared fences): the game's thread waits for neither");
        else
            HWARN("gfx: the hand-overs with DXVK wait on the game's thread (%s)", why.c_str());
    }
    HookState(g_dev9);
    g_generation++;
    g_state = "on";
    return true;
}

// A texture both devices use, as a render target of Direct3D 9's: on Direct3D 9Ex one texture, made by 9Ex with a
// shared handle and opened by Direct3D 11 (link null); on DXVK two, the Direct3D 11 one imported into DXVK's device
// and copied at the hand-overs (link set). False and why when it cannot be made.
bool MakePair(UINT w, UINT h, D3DFORMAT f9, IDirect3DTexture9 **t9, ID3D11Texture2D **t11, gfxvk::Link **link, std::string *why) {
    *link = nullptr;
    HANDLE sh = nullptr;
    const bool dxvk = gfxvk::On();
    HRESULT hr = g_dev9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f9, D3DPOOL_DEFAULT, t9, dxvk ? nullptr : &sh);
    if (FAILED(hr) || (!dxvk && !sh)) { *why = Fmt("Direct3D 9 made no texture to share (0x%08X)", static_cast<unsigned>(hr)); return false; }
    if (dxvk) {
        *link = gfxvk::Make(*t9, g_dev11, t11, why);
        return *link != nullptr;
    }
    hr = g_dev11->OpenSharedResource(sh, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(t11));
    if (FAILED(hr)) { *why = Fmt("Direct3D 11 could not open it (0x%08X)", static_cast<unsigned>(hr)); return false; }
    return true;
}
bool MakeShared(D3DFORMAT f9, IDirect3DTexture9 **t9, ID3D11Texture2D **t11, gfxvk::Link **link, const char *what) {
    std::string why;
    if (!MakePair(g_w, g_h, f9, t9, t11, link, &why)) return Fail(Fmt("the shared %s texture: %s", what, why.c_str()));
    return true;
}

// The motion vectors' targets at the frame's size, made at the first frame a mod wants them (motion.cpp); a failure is
// said once a size (gfx.state's motion.error) and not tried again until the size changes.
void FullScreen9(IDirect3DPixelShader9 *ps, IDirect3DBaseTexture9 *tex, const float c[8]);
bool EnsureMotion() {
    if (g_motionObj9 && g_motion9 && g_motionSRV && g_psMotionAll) return true;
    if (g_motionTried) return false;
    g_motionTried = true;
    std::string why;
    if (!g_psMotionAll && FAILED(g_dev9->CreatePixelShader(reinterpret_cast<const DWORD *>(kPsMotionAll), &g_psMotionAll))) why = "PsMotionAll";
    else if (FAILED(g_dev9->CreateTexture(g_w, g_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_motionObj9, nullptr)))
        why = "the moving objects' target";
    else if (!MakePair(g_w, g_h, D3DFMT_G16R16F, &g_motion9, &g_motion11, &g_motionLink, &why))
        why = "the shared motion vectors: " + why;
    else if (FAILED(g_dev11->CreateShaderResourceView(g_motion11, nullptr, &g_motionSRV)))
        why = "the motion vectors' view";
    if (!why.empty()) {
        g_motionError = why;
        HWARN("gfx: no motion vectors: %s", why.c_str());
        gfxvk::Free(g_motionLink);
        SafeRelease(g_motionSRV); SafeRelease(g_motion11); SafeRelease(g_motion9); SafeRelease(g_motionObj9);
        return false;
    }
    g_motionObj9->GetSurfaceLevel(0, &g_motionObjSurf9);
    g_motion9->GetSurfaceLevel(0, &g_motionSurf9);
    g_motionError.clear();
    HLOG("gfx: motion vectors %ux%u (the camera's and the moving objects')", g_w, g_h);
    return true;
}

bool Invert(const D3DMATRIX &a, D3DMATRIX &out) {   // (a 4x4 by Gauss-Jordan, partial pivoting)
    double m[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; j++) std::swap(m[c][j], m[p][j]);
        double inv = 1 / m[c][c];
        for (int j = 0; j < 8; j++) m[c][j] *= inv;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = m[r][c];
                for (int j = 0; j < 8; j++) m[r][j] -= f * m[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out.m[i][j] = static_cast<float>(m[i][j + 4]);
    return true;
}
void MulM(const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX &out) {
    D3DMATRIX r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    out = r;
}

// The frame's motion vectors (PsMotionAll, inside DoStage1: the bridge's states saved): the moving objects' where they
// are what is seen, the camera's elsewhere (this frame's view space through the last frame's camera).
bool MotionCombine(const D3DMATRIX &proj) {
    D3DMATRIX view{}, inv, before, viewBefore, projBefore;
    g_dev9->GetTransform(D3DTS_VIEW, &view);
    if (!Invert(view, inv)) return false;
    memcpy(&viewBefore, g_viewBefore, sizeof viewBefore);
    memcpy(&projBefore, g_projBefore, sizeof projBefore);
    if (g_jitWorld) JitterInto(projBefore, g_jitNow, 1);   // (this frame's jitter in both places: it cancels)
    MulM(inv, viewBefore, before);
    MulM(before, projBefore, before);
    // s1, the linear depth (written just before), with its states kept: the bridge's block holds stage 0's only
    IDirect3DBaseTexture9 *tex1 = nullptr;
    DWORD smp[5] = {};
    const D3DSAMPLERSTATETYPE ss[5] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    g_dev9->GetTexture(1, &tex1);
    for (int i = 0; i < 5; i++) g_dev9->GetSamplerState(1, ss[i], &smp[i]);
    g_dev9->SetTexture(1, g_depth9);
    const DWORD want[5] = {D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP};
    for (int i = 0; i < 5; i++) g_dev9->SetSamplerState(1, ss[i], want[i]);
    g_dev9->SetRenderTarget(0, g_motionSurf9);
    const D3DVIEWPORT9 vp{0, 0, g_dw, g_dh, 0, 1};   // (the world's part: the render size's under an upscaler)
    g_dev9->SetViewport(&vp);
    const float c[8] = {static_cast<float>(g_w), static_cast<float>(g_h), 1.0f / g_w, 1.0f / g_h, proj._33, proj._43, kFarDepth, 0};
    const float p[4] = {proj._11, proj._22, proj._31, proj._32};
    float drawn[4];
    GfxDrawnSize(drawn);
    g_dev9->SetPixelShaderConstantF(3, drawn, 1);
    g_dev9->SetPixelShaderConstantF(8, &before._11, 4);
    g_dev9->SetPixelShaderConstantF(12, p, 1);
    FullScreen9(g_psMotionAll, g_motionObj9, c);
    g_dev9->SetTexture(1, tex1);
    for (int i = 0; i < 5; i++) g_dev9->SetSamplerState(1, ss[i], smp[i]);
    SafeRelease(tex1);
    g_motionFrames++;
    return true;
}

// The hand-overs on DXVK (nothing to do on 9Ex, where both devices draw into one texture): Direct3D 9's pictures to
// Direct3D 11 once Direct3D 9 has finished them (Wait9), Direct3D 11's back once it has (Wait11).
bool Hand(bool to11, std::initializer_list<gfxvk::Link *> links) {
    if (!gfxvk::On()) return true;
    std::vector<gfxvk::Link *> v;
    for (gfxvk::Link *l : links) if (l) v.push_back(l);
    if (v.empty()) return true;
    std::string why;
    bool ok = to11 ? gfxvk::To11(v.data(), static_cast<int>(v.size()), &why) : gfxvk::To9(v.data(), static_cast<int>(v.size()), &why);
    if (!ok) g_frameError = "the textures' copy on DXVK's queue: " + why, g_frameLate = gfxvk::Busy();
    return ok;
}

bool EnsureSize(UINT w, UINT h) {
    if (w == g_w && h == g_h && g_intz) return true;
    ReleaseSized();
    g_w = w;
    g_h = h;
    if (FAILED(g_dev9->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, kFmtIntz, D3DPOOL_DEFAULT, &g_intz, nullptr))) return Fail("the INTZ depth texture");
    g_intz->GetSurfaceLevel(0, &g_intzSurf);
    if (!MakeShared(D3DFMT_A8R8G8B8, &g_color9, &g_color11, &g_colorLink, "colour") ||
        !MakeShared(D3DFMT_R32F, &g_depth9, &g_depth11, &g_depthLink, "depth") ||
        !MakeShared(D3DFMT_A16B16G16R16F, &g_fx9, &g_fx11, &g_fxLink, "effects"))
        return false;
    g_color9->GetSurfaceLevel(0, &g_colorSurf9);
    g_depth9->GetSurfaceLevel(0, &g_depthSurf9);
    if (FAILED(g_dev11->CreateShaderResourceView(g_color11, nullptr, &g_colorSRV)) ||
        FAILED(g_dev11->CreateShaderResourceView(g_depth11, nullptr, &g_depthSRV)) ||
        FAILED(g_dev11->CreateRenderTargetView(g_fx11, nullptr, &g_fxRTV)))
        return Fail("the shared textures' views");
    HLOG("gfx: frame %ux%u: a readable depth buffer, the shared colour, depth and effects textures", w, h);
    return true;
}

// ---- the devices wait for each other (a failure goes to g_frameError: the frame's states come back first)
// An issued event query polled up to limitMs (0: once); not done then: g_frameLate, and the query stays pending.
bool Poll9(double limitMs) {
    double t0 = NowMs();
    for (;;) {
        HRESULT hr = g_event9->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        g_pending9 = hr == S_FALSE;
        if (hr == S_OK) return true;
        if (hr != S_FALSE) { g_frameError = Fmt("the Direct3D 9 event query failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
        if (NowMs() - t0 >= limitMs) { g_frameError = "Direct3D 9 did not finish the frame's copy in time"; g_frameLate = true; return false; }
        SwitchToThread();
    }
}
bool Poll11(double limitMs) {
    double t0 = NowMs();
    for (;;) {
        HRESULT hr = g_ctx->GetData(g_event11, nullptr, 0, 0);
        g_pending11 = hr == S_FALSE;
        if (hr == S_OK) return true;
        if (FAILED(hr)) { g_frameError = Fmt("the Direct3D 11 event query failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
        if (NowMs() - t0 >= limitMs) { g_frameError = "Direct3D 11 did not finish the passes in time"; g_frameLate = true; return false; }
        SwitchToThread();
    }
}
bool Wait9() {
    double t0 = NowMs();
    g_event9->Issue(D3DISSUE_END);
    bool ok = Poll9(WaitLimitMs());
    g_tm.wait9Ms += NowMs() - t0;
    return ok;
}
bool Wait11() {
    double t0 = NowMs();
    g_ctx->End(g_event11);
    g_ctx->Flush();
    bool ok = Poll11(WaitLimitMs());
    g_tm.wait11Ms += NowMs() - t0;
    return ok;
}
// The devices' work ordered on the GPU (DXVK with the shared fences): no wait for either on the game's thread.
bool GpuSync() { return gfxvk::Gpu(); }
// A late frame's work done on both devices and on DXVK's queue (looked at once, not waited for): the shared textures
// may be written again. False: still running (g_frameLate), or a query failed.
bool Settled() {
    if (g_pending9 && !Poll9(0)) return false;
    if (g_pending11 && !Poll11(0)) return false;
    if (gfxvk::On() && gfxvk::Busy()) { g_frameError = "the copy on DXVK's queue did not finish in time"; g_frameLate = true; return false; }
    return true;
}

// A frame (or a view) without the passes: a late one is let go, the bridge goes on, unless the devices have been late
// for kLateGiveUpMs in a row; any other failure turns the bridge off.
bool FrameFailed() {
    std::string why = g_frameError.empty() ? std::string("the frame failed") : g_frameError;
    if (!g_frameLate) return Fail(why);
    double now = NowMs();
    g_lateFrames++;
    if (!g_lateRun++) {
        g_lateSince = now;
        g_lateRuns++;
        if (g_lateRuns <= kLateLogged) HWARN("gfx: %s: the frame goes without the passes", why.c_str());
        if (g_lateRuns == kLateLogged) HWARN("gfx: late frames %u times: more are counted in gfx.state, not logged", kLateLogged);
    }
    if (now - g_lateSince >= kLateGiveUpMs) return Fail(Fmt("%s (late for %.0f s, %llu frames)", why.c_str(), (now - g_lateSince) / 1000,
                                                            static_cast<unsigned long long>(g_lateRun)));
    return true;
}
void FrameOnTime() {
    if (!g_lateRun) return;
    if (g_lateRuns <= kLateLogged)
        HLOG("gfx: the devices caught up after %llu late frames (%.0f ms)", static_cast<unsigned long long>(g_lateRun), NowMs() - g_lateSince);
    g_lateRun = 0;
}

// ---- the frame
void FullScreen9(IDirect3DPixelShader9 *ps, IDirect3DBaseTexture9 *tex, const float c[8]) {
    g_dev9->SetVertexShader(g_vsQuad);
    g_dev9->SetPixelShader(ps);
    g_dev9->SetVertexDeclaration(g_decl);
    g_dev9->SetStreamSource(0, g_quad, 0, 16);
    g_dev9->SetTexture(0, tex);
    g_dev9->SetPixelShaderConstantF(0, c, 2);
    g_dev9->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    g_dev9->SetTexture(0, nullptr);
}

// The constants of the picture drawn now (the frame's or a view's: its size; the device's transforms are its camera);
// its view and projection to viewOut, projOut.
void FillConstants(MmlGfxConstants &k, UINT w, UINT h, float *viewOut, float *projOut) {
    D3DMATRIX v{}, p{};
    g_dev9->GetTransform(D3DTS_VIEW, &v);
    g_dev9->GetTransform(D3DTS_PROJECTION, &p);
    if (g_jitWorld && !g_viewNow) JitterInto(p, g_jitNow, -1);   // (the camera's own: the jitter is in MmlGfxFrame's)
    // a row-vector view matrix: its columns are the camera's axes in the world; its fourth row -(eye . axis)
    float r[3] = {v._11, v._21, v._31}, u[3] = {v._12, v._22, v._32}, f[3] = {v._13, v._23, v._33};
    for (int i = 0; i < 3; i++) {
        k.right[i] = r[i];
        k.up[i] = u[i];
        k.fwd[i] = f[i];
        k.eye[i] = -(v._41 * r[i] + v._42 * u[i] + v._43 * f[i]);
    }
    k.eye[3] = static_cast<float>(NowMs() / 1000.0);
    k.right[3] = k.up[3] = k.fwd[3] = 0;
    k.proj[0] = p._11; k.proj[1] = p._22; k.proj[2] = p._31; k.proj[3] = p._32;
    k.size[0] = static_cast<float>(w); k.size[1] = static_cast<float>(h); k.size[2] = 1.0f / w; k.size[3] = 1.0f / h;
    // the sun: the light the world was drawn with (Tally); none: a default from above
    float s[3] = {0.35f, 0.85f, 0.4f}, sc[3] = {1, 1, 1};
    bool fromGame = false;
    if (g_haveWorldSun) {
        s[0] = -g_worldSun.dir.x; s[1] = -g_worldSun.dir.y; s[2] = -g_worldSun.dir.z;
        sc[0] = g_worldSun.color.r; sc[1] = g_worldSun.color.g; sc[2] = g_worldSun.color.b;
        fromGame = s[0] * s[0] + s[1] * s[1] + s[2] * s[2] > 1e-12f;
        if (!fromGame) { s[0] = 0.35f; s[1] = 0.85f; s[2] = 0.4f; sc[0] = sc[1] = sc[2] = 1; }
    }
    // no Direct3D light in the frame: the scene's own sun and ambient light (lights.cpp: LS3D's light frames), read
    // at most every 100 ms
    if (GetTickCount() - g_sceneSunAt >= 100 || !g_sceneSunAt) {
        g_sceneSunAt = GetTickCount();
        g_haveSceneSun = SceneSun(g_sceneSun, g_sceneSunColor, g_sceneAmbient) != 0;
    }
    bool fromScene = false;
    if (!fromGame && g_haveSceneSun) {
        for (int i = 0; i < 3; i++) { s[i] = g_sceneSun[i]; sc[i] = g_sceneSunColor[i]; }
        fromGame = fromScene = s[0] * s[0] + s[1] * s[1] + s[2] * s[2] > 1e-12f;
        if (!fromGame) { s[0] = 0.35f; s[1] = 0.85f; s[2] = 0.4f; sc[0] = sc[1] = sc[2] = 1; }
    }
    g_sunFrom = fromScene ? "the scene's lights" : fromGame ? "Direct3D's light" : "a default from above";
    float n = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (int i = 0; i < 3; i++) { k.sun[i] = s[i] / n; k.sun_color[i] = sc[i]; k.ambient[i] = g_haveSceneSun ? g_sceneAmbient[i] : 0.5f; }
    k.sun[3] = k.sun_color[3] = fromGame ? 1.0f : 0.0f;
    k.ambient[3] = g_haveSceneSun ? 1.0f : 0.0f;   // (the scene's ambient light: LS3D's own, not a Direct3D state)
    k.depth_proj[0] = p._33; k.depth_proj[1] = p._43; k.depth_proj[2] = kFarDepth; k.depth_proj[3] = 0;
    DWORD on = 0, color = 0, table = 0, vertex = 0, start = 0, end = 0, density = 0;
    if (g_stateHooked && g_haveWorldFog) {
        const Fog &w = g_worldFog;
        on = w.on; color = w.color; table = w.table; vertex = w.vertex; start = w.start; end = w.end; density = w.density;
    } else {
        g_dev9->GetRenderState(D3DRS_FOGENABLE, &on);
        g_dev9->GetRenderState(D3DRS_FOGCOLOR, &color);
        g_dev9->GetRenderState(D3DRS_FOGTABLEMODE, &table);
        g_dev9->GetRenderState(D3DRS_FOGVERTEXMODE, &vertex);
        g_dev9->GetRenderState(D3DRS_FOGSTART, &start);
        g_dev9->GetRenderState(D3DRS_FOGEND, &end);
        g_dev9->GetRenderState(D3DRS_FOGDENSITY, &density);
    }
    DWORD mode = table != D3DFOG_NONE ? table : vertex;
    memcpy(&k.fog[0], &start, 4);
    memcpy(&k.fog[1], &end, 4);
    memcpy(&k.fog[2], &density, 4);
    k.fog[3] = static_cast<float>(mode == D3DFOG_EXP ? 1 : mode == D3DFOG_EXP2 ? 2 : mode == D3DFOG_LINEAR ? 3 : 0);
    k.fog_color[0] = ((color >> 16) & 255) / 255.0f;
    k.fog_color[1] = ((color >> 8) & 255) / 255.0f;
    k.fog_color[2] = (color & 255) / 255.0f;
    k.fog_color[3] = on && mode != D3DFOG_NONE ? 1.0f : 0.0f;
    memcpy(viewOut, &v, sizeof v);
    memcpy(projOut, &p, sizeof p);
}

// What the passes draw over: the frame (view 0, every pass, timed) or a scene view (only gfx_pass_views' passes).
struct PassTarget {
    ID3D11ShaderResourceView *depth, *color;
    ID3D11RenderTargetView *rtv;
    UINT w, h;
    int view;
    const float *viewM, *projM;
};

struct PassCall { const Pass *pass; const MmlGfxFrame *frame; };
void CallPass(void *p) {
    auto *c = static_cast<PassCall *>(p);
    c->pass->fn(c->frame, c->pass->user);
}

// The sun's shadow map for this frame's passes (shadows.cpp drew it before the world, on Direct3D 9: done by the time
// stage 1 has waited for Direct3D 9), opened again when shadows.cpp made a new one or the device is new; its matrix
// and size into the constants. Null: none this frame.
ID3D11ShaderResourceView *SunShadow(MmlGfxConstants &k, bool frame) {
    void *h = nullptr;
    unsigned made = 0;
    bool dxvk = gfxvk::On();
    if (!ShadowsShared(&h, &made, k.sun_shadow_view_proj, k.sun_shadow_info) || (!h && !dxvk)) {
        memset(k.sun_shadow_view_proj, 0, sizeof k.sun_shadow_view_proj);
        memset(k.sun_shadow_info, 0, sizeof k.sun_shadow_info);
        return nullptr;
    }
    if (dxvk) {   // DXVK: the map's Direct3D 11 half, made with the map, copied once a frame (the map is redrawn each)
        IDirect3DBaseTexture9 *base = ShadowsTexture();
        IDirect3DTexture9 *map = nullptr;
        if (!base || FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&map))) || !map) return nullptr;
        if (!g_sunSRV || map != g_sunHandle || made != g_sunMade || g_sunGen != g_generation) {
            gfxvk::Free(g_sunLink);
            SafeRelease(g_sunSRV);
            SafeRelease(g_sun11);
            g_sunHandle = map;
            g_sunMade = made;
            g_sunGen = g_generation;
            std::string why;
            g_sunLink = gfxvk::Make(map, g_dev11, &g_sun11, &why);
            HRESULT hr = g_sunLink ? g_dev11->CreateShaderResourceView(g_sun11, nullptr, &g_sunSRV) : E_FAIL;
            g_sunError = !g_sunLink ? "the sun's shadow map through Vulkan: " + why :
                         FAILED(hr) ? Fmt("the sun's shadow map's view (0x%08X)", static_cast<unsigned>(hr)) : "";
            if (!g_sunError.empty()) { gfxvk::Free(g_sunLink); SafeRelease(g_sunSRV); SafeRelease(g_sun11); }
        }
        map->Release();
        std::string why;
        if (g_sunSRV && frame && !gfxvk::To11(&g_sunLink, 1, &why)) g_sunError = "the sun's shadow map's copy: " + why;
        if (!g_sunSRV) k.sun_shadow_info[0] = 0;
        return g_sunSRV;
    }
    if (!g_sunSRV || h != g_sunHandle || made != g_sunMade || g_sunGen != g_generation) {
        SafeRelease(g_sunSRV);
        SafeRelease(g_sun11);
        g_sunHandle = h;
        g_sunMade = made;
        g_sunGen = g_generation;
        HRESULT hr = g_dev11->OpenSharedResource(h, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&g_sun11));
        if (SUCCEEDED(hr)) hr = g_dev11->CreateShaderResourceView(g_sun11, nullptr, &g_sunSRV);
        g_sunError = FAILED(hr) ? Fmt("Direct3D 11 could not open the sun's shadow map (0x%08X)", static_cast<unsigned>(hr)) : "";
        if (FAILED(hr)) { SafeRelease(g_sunSRV); SafeRelease(g_sun11); }
    }
    if (!g_sunSRV) k.sun_shadow_info[0] = 0;
    return g_sunSRV;
}

// The frame at its size for the passes after the upscaler (made at the first frame that needs them).
bool EnsureUp() {
    if (g_upColor11) return true;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_w, td.Height = g_h, td.MipLevels = 1, td.ArraySize = 1, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // (the effects target's: copied whole)
    bool ok = SUCCEEDED(g_dev11->CreateTexture2D(&td, nullptr, &g_upColor11)) &&
              SUCCEEDED(g_dev11->CreateShaderResourceView(g_upColor11, nullptr, &g_upColorSRV));
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    ok = ok && SUCCEEDED(g_dev11->CreateTexture2D(&td, nullptr, &g_upDepth11)) &&
         SUCCEEDED(g_dev11->CreateShaderResourceView(g_upDepth11, nullptr, &g_upDepthSRV)) &&
         SUCCEEDED(g_dev11->CreateRenderTargetView(g_upDepth11, nullptr, &g_upDepthRTV));
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    ok = ok && SUCCEEDED(g_dev11->CreateTexture2D(&td, nullptr, &g_upMotion11)) &&
         SUCCEEDED(g_dev11->CreateShaderResourceView(g_upMotion11, nullptr, &g_upMotionSRV)) &&
         SUCCEEDED(g_dev11->CreateRenderTargetView(g_upMotion11, nullptr, &g_upMotionRTV));
    if (!ok) {
        HWARN("gfx: the frame at its size for the passes after the upscaler could not be made: they get the smaller picture");
        SafeRelease(g_upColorSRV); SafeRelease(g_upDepthSRV); SafeRelease(g_upMotionSRV);
        SafeRelease(g_upDepthRTV); SafeRelease(g_upMotionRTV);
        SafeRelease(g_upColor11); SafeRelease(g_upDepth11); SafeRelease(g_upMotion11);
    }
    return ok;
}

// A bridge's own full-screen draw on Direct3D 11 (the upscaler's helpers): ps into rtv over the frame, srvs at t0..t3,
// g_up's sizes at b1, no blending.
void Draw11(ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *const srv[4]) {
    const float up[4] = {static_cast<float>(g_dw), static_cast<float>(g_dh), static_cast<float>(g_w), static_cast<float>(g_h)};
    g_ctx->UpdateSubresource(g_upCB, 0, nullptr, up, 0, 0);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_w), static_cast<float>(g_h), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->RSSetState(nullptr);
    g_ctx->PSSetShaderResources(0, 4, srv);
    ID3D11SamplerState *smp[2] = {g_point, g_linear};
    g_ctx->PSSetSamplers(0, 2, smp);
    g_ctx->PSSetConstantBuffers(1, 1, &g_upCB);
    g_ctx->VSSetShader(g_vsFull, nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none[4] = {};
    g_ctx->PSSetShaderResources(0, 4, none);
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

struct UpscaleCall { const MmlGfxFrame *frame; int drew; };
void CallUpscale(void *p) {
    auto *c = static_cast<UpscaleCall *>(p);
    c->drew = g_up.fn(c->frame, g_up.user);
}
// The upscaler first (gfx_upscale): the whole frame into the effects target from the picture drawn at the render size
// (stretched by the bridge when it drew nothing), then the frame at its size in f for the passes after it.
void RunUpscaler(MmlGfxConstants &k, const PassTarget &t, MmlGfxFrame &f, ID3D11ShaderResourceView *srv[4], TimerFrame *tf) {
    const bool scaled = g_dw != g_w || g_dh != g_h;
    MmlGfxConstants ku = k;   // (its pixels: the render size's)
    ku.size[0] = static_cast<float>(g_dw), ku.size[1] = static_cast<float>(g_dh), ku.size[2] = 1.0f / g_dw, ku.size[3] = 1.0f / g_dh;
    g_ctx->UpdateSubresource(g_frameCB, 0, nullptr, &ku, 0, 0);
    MmlGfxFrame fu = f;
    fu.k = &ku;
    fu.render_width = g_dw, fu.render_height = g_dh;
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(t.w), static_cast<float>(t.h), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
    g_ctx->OMSetBlendState(g_over, nullptr, 0xFFFFFFFFu);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->RSSetState(nullptr);
    ID3D11SamplerState *smp[2] = {g_point, g_linear};
    g_ctx->PSSetShaderResources(0, 4, srv);
    g_ctx->PSSetSamplers(0, 2, smp);
    g_ctx->PSSetConstantBuffers(0, 1, &g_frameCB);
    g_ctx->CSSetShaderResources(0, 4, srv);
    g_ctx->CSSetSamplers(0, 2, smp);
    g_ctx->CSSetConstantBuffers(0, 1, &g_frameCB);
    g_ctx->VSSetConstantBuffers(0, 1, &g_frameCB);
    g_ctx->VSSetShader(g_vsFull, nullptr, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    UpscaleCall call{&fu, 0};
    double t0 = NowMs();
    ModCall(g_up.mod, CallUpscale, &call, "its upscaler (gfx_upscale)");
    double ms = NowMs() - t0;
    g_upCpuMs = g_upCpuMs <= 0 ? ms : g_upCpuMs * 0.9 + ms * 0.1;
    g_ctx->ClearState();
    if (call.drew > 0) {
        g_upDrawn++;
    } else if (scaled) {   // (nothing of the upscaler's: the smaller picture stretched over the frame)
        ID3D11ShaderResourceView *in[4] = {nullptr, f.color ? static_cast<ID3D11ShaderResourceView *>(f.color) : nullptr, nullptr, nullptr};
        Draw11(g_psStretch, t.rtv, in);
        g_upStretched++;
    }
    Mark(tf, -1);
    g_ctx->UpdateSubresource(g_frameCB, 0, nullptr, &k, 0, 0);   // (the passes after it: the frame's pixels)
    // the frame at its size for the passes after it
    bool others = false;
    for (const Pass &p : g_passes) others = others || ModAlive(p.mod);
    g_upMade = false;
    if (!others || !(call.drew > 0 || scaled) || !EnsureUp()) return;
    ID3D11Resource *fx = nullptr;
    t.rtv->GetResource(&fx);
    if (fx) { g_ctx->CopyResource(g_upColor11, fx); fx->Release(); }
    f.color = g_upColorSRV;
    srv[1] = g_upColorSRV;
    if (scaled) {
        ID3D11ShaderResourceView *in[4] = {srv[0], nullptr, nullptr, srv[3]};
        Draw11(g_psUpDepth, g_upDepthRTV, in);
        f.depth = srv[0] = g_upDepthSRV;
        if (srv[3]) {
            Draw11(g_psUpMotion, g_upMotionRTV, in);
            f.motion = srv[3] = g_upMotionSRV;
        }
    }
    g_upMade = true;
}

bool RunPasses(MmlGfxConstants &k, const PassTarget &t) {
    bool frame = t.view == 0;
    if (frame) ReadTimers();
    ID3D11ShaderResourceView *sun = SunShadow(k, frame);
    const float clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(t.rtv, clear);
    g_ctx->UpdateSubresource(g_frameCB, 0, nullptr, &k, 0, 0);
    MmlGfxFrame f{};
    f.size = sizeof f;
    f.device = g_dev11;
    f.context = g_ctx;
    f.generation = g_generation;
    f.k = &k;
    f.constants = g_frameCB;
    f.depth = t.depth;
    f.color = t.color;
    f.point = g_point;
    f.linear = g_linear;
    f.target = t.rtv;
    f.full_screen = g_vsFull;
    f.width = t.w;
    f.height = t.h;
    memcpy(f.view, t.viewM, sizeof f.view);
    memcpy(f.proj, t.projM, sizeof f.proj);
    if (frame && UiMode() != MML_GFX_UI_OFF && g_uiFrames) f.ui = g_uiSRV;
    f.ui_frames = frame ? g_uiFrames : 0;
    std::vector<MmlGfxView> views;   // (a view's passes get no views)
    if (frame) for (auto &v : g_views) if (v->info.frames && v->colorSRV) views.push_back(v->info);
    f.views = views.empty() ? nullptr : views.data();
    f.view_count = static_cast<unsigned>(views.size());
    f.in_view = t.view;
    f.sun_shadow = sun;
    ID3D11ShaderResourceView *motion = frame && g_motionReady ? g_motionSRV : nullptr;   // (t3: the frame's only)
    f.motion = motion;
    f.jitter[0] = frame ? g_jitNow[0] : 0, f.jitter[1] = frame ? g_jitNow[1] : 0;
    f.render_width = t.w, f.render_height = t.h;
    TimerFrame *tf = frame ? BeginTimers() : nullptr;
    if (tf) Mark(tf, 0);
    g_inPasses = true;
    ID3D11ShaderResourceView *srv[4] = {t.depth, t.color, sun, motion};
    g_upMade = false;
    if (frame && g_upFrame && g_up.fn && ModAlive(g_up.mod)) RunUpscaler(k, t, f, srv, tf);
    std::vector<Pass> passes = g_passes;   // (a pass may add or take back passes)
    for (const Pass &p : passes) {
        if (!ModAlive(p.mod) || (!frame && !p.inViews)) continue;
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(t.w), static_cast<float>(t.h), 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
        g_ctx->OMSetBlendState(g_over, nullptr, 0xFFFFFFFFu);   // (a pass's 0 leaves the passes before it as they are)
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->RSSetState(nullptr);
        ID3D11SamplerState *smp[2] = {g_point, g_linear};
        g_ctx->PSSetShaderResources(0, 4, srv);
        g_ctx->PSSetSamplers(0, 2, smp);
        g_ctx->PSSetConstantBuffers(0, 1, &g_frameCB);
        g_ctx->CSSetShaderResources(0, 4, srv);
        g_ctx->CSSetSamplers(0, 2, smp);
        g_ctx->CSSetConstantBuffers(0, 1, &g_frameCB);
        g_ctx->VSSetConstantBuffers(0, 1, &g_frameCB);
        g_ctx->VSSetShader(g_vsFull, nullptr, 0);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->IASetInputLayout(nullptr);
        PassCall call{&p, &f};
        double t0 = NowMs();
        ModCall(p.mod, CallPass, &call, frame ? "its graphics pass" : "its graphics pass (in a scene view)");
        double ms = NowMs() - t0;
        if (!frame) continue;
        for (Pass &q : g_passes)
            if (q.handle == p.handle) q.cpuMs = q.cpuMs <= 0 ? ms : q.cpuMs * 0.9 + ms * 0.1;
        Mark(tf, p.handle);
    }
    g_inPasses = false;
    if (tf) EndTimers(tf);
    g_ctx->ClearState();   // the shared textures are Direct3D 9's again
    if (GpuSync()) return true;   // (Hand's To9 queues Direct3D 11's signal and DXVK's wait for it)
    double t0 = NowMs();
    bool ok = Wait11();
    double w = NowMs() - t0;
    g_wait11 = g_wait11 <= 0 ? w : g_wait11 * 0.9 + w * 0.1;
    return ok;
}

// The bridge's own drawing inside LS3D's frame: what the block does not hold is saved by hand, the block captures
// the states the bridge changes, and those are set to plain values; Restore9 gives everything back.
struct Saved9 {
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9 *vb = nullptr;
    UINT vbOff = 0, vbStride = 0, freq = 1;
};
void SaveAndNeutral9(Saved9 &s) {
    g_dev9->GetVertexShader(&s.vs);
    g_dev9->GetPixelShader(&s.ps);
    g_dev9->GetVertexDeclaration(&s.decl);
    g_dev9->GetFVF(&s.fvf);
    g_dev9->GetStreamSource(0, &s.vb, &s.vbOff, &s.vbStride);
    g_dev9->GetStreamSourceFreq(0, &s.freq);
    g_block->Capture();
    g_dev9->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev9->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    g_dev9->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    g_dev9->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_dev9->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    g_dev9->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_dev9->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g_dev9->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev9->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev9->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_dev9->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    g_dev9->SetStreamSourceFreq(0, 1);
}
void Restore9(Saved9 &s) {
    g_block->Apply();
    g_dev9->SetVertexShader(s.vs);
    g_dev9->SetPixelShader(s.ps);
    if (s.fvf) g_dev9->SetFVF(s.fvf);
    else g_dev9->SetVertexDeclaration(s.decl);
    g_dev9->SetStreamSource(0, s.vb, s.vbOff, s.vbStride);
    g_dev9->SetStreamSourceFreq(0, s.freq);
    SafeRelease(s.vs); SafeRelease(s.ps); SafeRelease(s.decl); SafeRelease(s.vb);
}

// The world drawn smaller and no upscaled frame over it (a frame the passes did not run): the smaller picture stretched
// over the frame by Direct3D 9 (through the colour's texture: StretchRect takes no overlapping copy), so the frame is
// never seen in a corner.
void Unscaled9(IDirect3DSurface9 *rt, bool copied) {
    if (!g_upFrame || (g_dw == g_w && g_dh == g_h) || !rt || !g_colorSurf9) return;
    RECT r{0, 0, static_cast<LONG>(g_dw), static_cast<LONG>(g_dh)};
    if (!copied) g_dev9->StretchRect(rt, &r, g_colorSurf9, &r, D3DTEXF_NONE);
    g_dev9->StretchRect(g_colorSurf9, &r, rt, nullptr, D3DTEXF_LINEAR);
    g_upStretched++;
}

bool DoStage1() {
    double t0 = NowMs();
    IDirect3DSurface9 *rt = nullptr;
    if (FAILED(g_dev9->GetRenderTarget(0, &rt)) || !rt) { g_skipped++; return true; }
    D3DSURFACE_DESC rd;
    rt->GetDesc(&rd);
    IDirect3DSurface9 *ds = nullptr;
    g_dev9->GetDepthStencilSurface(&ds);
    bool ours = ds == g_intzSurf;
    SafeRelease(ds);
    if (!ours || rd.Width != g_w || rd.Height != g_h) {   // LS3D bound another depth buffer or target this frame
        if (ours) g_skipped++; else g_notOurDepth++;
        Unscaled9(rt, false);
        SafeRelease(rt);
        return true;
    }
    if (!Settled()) {   // a late frame's work still runs on the shared textures
        Unscaled9(rt, false);
        SafeRelease(rt);
        MotionFrameEnd();
        g_motionThisFrame = g_motionReady = false;
        return FrameFailed();
    }
    Saved9 saved;
    SaveAndNeutral9(saved);
    // 1. the colour and the linear depth into the shared textures (the world's part of them: the render size's under
    // an upscaler, at their top-left)
    const D3DVIEWPORT9 drawn{0, 0, g_dw, g_dh, 0, 1};
    D3DMATRIX proj{};
    g_dev9->GetTransform(D3DTS_PROJECTION, &proj);
    const float c[8] = {static_cast<float>(g_w), static_cast<float>(g_h), 1.0f / g_w, 1.0f / g_h, proj._33, proj._43, kFarDepth, 0};
    RECT region{0, 0, static_cast<LONG>(g_dw), static_cast<LONG>(g_dh)};
    bool ok = SUCCEEDED(g_dev9->StretchRect(rt, &region, g_colorSurf9, &region, D3DTEXF_NONE));
    if (!ok) g_frameError = "StretchRect of the frame's colour failed";
    g_dev9->SetRenderTarget(0, g_depthSurf9);
    g_dev9->SetDepthStencilSurface(nullptr);   // the INTZ texture is read now: not bound for writing
    g_dev9->SetViewport(&drawn);
    FullScreen9(g_psDepth, g_intz, c);
    // the motion vectors, when the moving objects were drawn again this frame (motion.cpp)
    g_motionReady = false;
    bool motion = g_motionThisFrame && g_motionSurf9 && g_psMotionAll && MotionCombine(proj);
    MotionFrameEnd();
    g_motionThisFrame = false;
    {   // this frame's camera, for the next frame's motion
        D3DMATRIX v{};
        g_dev9->GetTransform(D3DTS_VIEW, &v);
        D3DMATRIX clean = proj;
        if (g_jitWorld) JitterInto(clean, g_jitNow, -1);
        memcpy(g_viewBefore, &v, sizeof g_viewBefore);
        memcpy(g_projBefore, &clean, sizeof g_projBefore);
        g_cameraBefore = true;
    }
    ok = ok && (GpuSync() || Wait9());
    if (ok && gfxvk::On()) {   // (on DXVK: the pictures copied to Direct3D 11's halves, the 2D pass's and the views' too)
        std::vector<gfxvk::Link *> to11{g_colorLink, g_depthLink};
        if (motion) to11.push_back(g_motionLink);
        if (UiMode() != MML_GFX_UI_OFF && g_uiFrames) to11.push_back(g_uiLink);
        for (auto &v : g_views)
            if (v->info.fresh) { to11.push_back(v->colorLink); to11.push_back(v->depthLink); }
        std::string why;
        ok = gfxvk::To11(to11.data(), static_cast<int>(to11.size()), &why);
        if (!ok) g_frameError = "the textures' copy on DXVK's queue: " + why, g_frameLate = gfxvk::Busy();
    }
    g_motionReady = ok && motion;
    double t1 = NowMs();
    // 2. the upscaler and the passes
    if (ok) {
        FillConstants(g_k, g_w, g_h, g_viewM, g_projM);
        ok = RunPasses(g_k, {g_depthSRV, g_colorSRV, g_fxRTV, g_w, g_h, 0, g_viewM, g_projM});
    }
    ok = ok && Hand(false, {g_fxLink});
    double t2 = NowMs();
    // 3. the effects over the frame, then everything back
    g_dev9->SetRenderTarget(0, rt);
    g_dev9->SetDepthStencilSurface(g_intzSurf);
    if (ok) {
        g_dev9->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_dev9->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_dev9->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_dev9->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        FullScreen9(g_psComposite, g_fx9, c);
    } else {
        Unscaled9(rt, true);
    }
    Restore9(saved);
    SafeRelease(rt);
    if (!ok) return FrameFailed();
    FrameOnTime();
    g_frames++;
    auto smooth = [](double &avg, double v) { avg = g_frames == 1 ? v : avg * 0.95 + v * 0.05; };
    smooth(g_ms9, t1 - t0);
    smooth(g_ms11, t2 - t1);
    smooth(g_msAll, NowMs() - t0);
    return true;
}

std::string g_faultWhere;
int NoteFault(EXCEPTION_POINTERS *ep) {
    g_faultWhere = Fmt("exception %08X at %s", static_cast<unsigned>(ep->ExceptionRecord->ExceptionCode),
                       DescribeAddress(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)).c_str());
    return EXCEPTION_EXECUTE_HANDLER;
}
bool SafeStage1() { __try { return DoStage1(); } __except (NoteFault(GetExceptionInformation())) { return false; } }

bool Wants() {
    for (const Pass &p : g_passes) if (ModAlive(p.mod)) return !g_failed;
    return g_up.fn && ModAlive(g_up.mod) && !g_failed;
}

void *Device8() {
    return RenderDevice(8);
}

// ---- the game's device made without multisampling for the bridge's mods (a readable depth buffer cannot be
// multisampled). Direct3DCreate8 (LS3D's import of d3d8.dll: dxwrapper's d3d8to9 in the labs) gives the IDirect3D8
// whose CreateDevice (vtable slot 15) makes the device; its Reset (the device's slot 14) makes it again with new
// parameters (another resolution in the options). Both take D3DPRESENT_PARAMETERS (Direct3D 8: 13 dwords, the
// multisample type the fifth). The game's own setting (in the registry, shared by every copy) is left alone.
using Direct3DCreate8Fn = void *(WINAPI *)(UINT);
using CreateDevice8Fn = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, DWORD, HWND, DWORD, DWORD *, void **);
using Reset8Fn = HRESULT(STDMETHODCALLTYPE *)(void *, DWORD *);
Direct3DCreate8Fn oDirect3DCreate8;
CreateDevice8Fn oCreateDevice8;
Reset8Fn oReset8;
std::string g_msaa = "not seen (the device was made before the loader, or no Direct3DCreate8 import)";

void NoMultisampling(DWORD *pp, const char *when) {
    std::vector<std::string> mods = ModsWantingGfx();
    if (!pp) return;
    if (mods.empty()) {
        g_msaa = Fmt("the game's own (%ux, no mod with \"gfx\": true)", static_cast<unsigned>(pp[4]));
        return;
    }
    std::string list;
    for (const std::string &m : mods) list += (list.empty() ? "" : ", ") + m;
    if (pp[4]) HLOG("gfx: %s: multisampling %ux turned off for the graphics bridge (%s)", when, static_cast<unsigned>(pp[4]), list.c_str());
    g_msaa = Fmt("off for %s (the game asked %ux)", list.c_str(), static_cast<unsigned>(pp[4]));
    pp[4] = 0;
}
HRESULT STDMETHODCALLTYPE hkReset8(void *dev, DWORD *pp) {
    NoMultisampling(pp, "Reset");
    ShowThreadStop();   // (its swap chain and slots are the device's default pool: gone before a Reset)
    GammaReset();
    return oReset8(dev, pp);
}
HRESULT STDMETHODCALLTYPE hkCreateDevice8(void *self, UINT adapter, DWORD type, HWND focus, DWORD flags, DWORD *pp, void **device) {
    NoMultisampling(pp, "CreateDevice");
    if (!ModsWantingGfx().empty()) flags |= D3DCREATE_MULTITHREADED;   // (the showing's own thread presents on it)
    HRESULT hr = oCreateDevice8(self, adapter, type, focus, flags, pp, device);
    if (SUCCEEDED(hr) && device && *device) GammaDevice(*device);   // (gamma.cpp: the brightness in a window on DXVK)
    if (SUCCEEDED(hr) && device && *device && !oReset8) {
        std::string err;
        void *reset = (*reinterpret_cast<void ***>(*device))[14];
        if (!HookAdd(nullptr, reinterpret_cast<uintptr_t>(reset), reinterpret_cast<void *>(&hkReset8), reinterpret_cast<void **>(&oReset8), &err))
            HWARN("gfx: the device's Reset not hooked (%s): a new resolution may bring multisampling back", err.c_str());
    }
    return hr;
}
void *WINAPI hkDirect3DCreate8(UINT sdk) {
    if (!ModsWantingGfx().empty() && !LoadD3d11()) HWARN("gfx: Windows' d3d11.dll or dxgi.dll did not load from System32");
    void *d3d = oDirect3DCreate8(sdk);
    if (d3d && !oCreateDevice8) {
        std::string err;
        void *create = (*static_cast<void ***>(d3d))[15];
        if (!HookAdd(nullptr, reinterpret_cast<uintptr_t>(create), reinterpret_cast<void *>(&hkCreateDevice8), reinterpret_cast<void **>(&oCreateDevice8), &err))
            HWARN("gfx: IDirect3D8::CreateDevice not hooked (%s): the game's multisampling stays", err.c_str());
    }
    return d3d;
}

// ---- the frame at its showing (gfx_show): a mod's pictures shown before the frame by the game's own Present, spaced
// evenly over the time between the game's frames (frame generation's in-between frames). The back buffer, whole (the
// HUD on it), is copied into a texture both devices share; the mod draws its pictures on the bridge's device into
// targets the bridge shares too; each is copied into the back buffer and shown, a wait between them, then the frame
// itself (copied back: the back buffer's content is gone after a Present). The waits are on the game's thread (only it
// may use the device and its back buffer): the game's frame takes them in.
struct ShowFn { MmlMod *mod = nullptr; MmlGfxShowFn fn = nullptr; void *user = nullptr; } g_showFn;
IDirect3DTexture9 *g_show9, *g_extra9[MML_GFX_SHOW_MAX];
IDirect3DSurface9 *g_showSurf9, *g_extraSurf9[MML_GFX_SHOW_MAX];
ID3D11Texture2D *g_show11, *g_extra11[MML_GFX_SHOW_MAX];
ID3D11ShaderResourceView *g_showSRV;
ID3D11RenderTargetView *g_extraRTV[MML_GFX_SHOW_MAX];
gfxvk::Link *g_showLink, *g_extraLink[MML_GFX_SHOW_MAX];
UINT g_showW, g_showH;
bool g_showTried;                  // made (or failed) at this size: a failure is not tried again every frame
std::string g_showError;
double g_showStart, g_showEnd;     // the last Present's start and end (ms)
double g_interval, g_work;         // smoothed: the time between the game's Presents, and of it the time outside them
double g_showMs;                   // smoothed: the bridge's own time on a frame with pictures (the copies, the mod's, the waits for the devices)
uint64_t g_shows, g_showFrames, g_showPictures, g_showLate;
HANDLE g_showTimer;
double g_lastShown;                // when the last picture or frame was handed to Present (ms)
double g_gap, g_gapErr;            // smoothed: the time between two things shown, and its distance from the spacing meant

void ReleaseShow() {
    ShowThreadStop();
    gfxvk::Free(g_showLink);
    SafeRelease(g_showSRV); SafeRelease(g_show11); SafeRelease(g_showSurf9); SafeRelease(g_show9);
    for (int i = 0; i < MML_GFX_SHOW_MAX; i++) {
        gfxvk::Free(g_extraLink[i]);
        SafeRelease(g_extraRTV[i]); SafeRelease(g_extra11[i]); SafeRelease(g_extraSurf9[i]); SafeRelease(g_extra9[i]);
    }
    g_showW = g_showH = 0;
    g_showTried = false;
}

// The shared textures at the back buffer's size: the frame (Direct3D 9 writes, 11 reads) and the pictures (11 writes,
// 9 reads), A8R8G8B8 (B8G8R8A8_UNORM on Direct3D 11).
bool EnsureShow(UINT w, UINT h) {
    if (w == g_showW && h == g_showH && g_showTried) return g_showSRV != nullptr;
    ReleaseShow();
    g_showW = w, g_showH = h;
    g_showTried = true;
    std::string why;
    bool ok = MakePair(w, h, D3DFMT_A8R8G8B8, &g_show9, &g_show11, &g_showLink, &why) &&
              SUCCEEDED(g_show9->GetSurfaceLevel(0, &g_showSurf9)) && SUCCEEDED(g_dev11->CreateShaderResourceView(g_show11, nullptr, &g_showSRV));
    for (int i = 0; ok && i < MML_GFX_SHOW_MAX; i++)
        ok = MakePair(w, h, D3DFMT_A8R8G8B8, &g_extra9[i], &g_extra11[i], &g_extraLink[i], &why) &&
             SUCCEEDED(g_extra9[i]->GetSurfaceLevel(0, &g_extraSurf9[i])) &&
             SUCCEEDED(g_dev11->CreateRenderTargetView(g_extra11[i], nullptr, &g_extraRTV[i]));
    if (!ok) {
        g_showError = "the showing's shared textures: " + (why.empty() ? std::string("a view was refused") : why);
        HWARN("gfx: %s", g_showError.c_str());
        ReleaseShow();
        g_showW = w, g_showH = h;
        g_showTried = true;
        return false;
    }
    g_showError.clear();
    HLOG("gfx: the showing at %ux%u: the frame and %d pictures shared", w, h, MML_GFX_SHOW_MAX);
    return true;
}

// Until targetMs (NowMs's clock): a high-resolution waitable timer for the most of it (one a thread), the last half
// millisecond spun.
void WaitUntil(double targetMs, HANDLE *timer) {
    double left = targetMs - NowMs();
    if (left <= 0) return;
    if (!*timer) *timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (*timer && left > 1.0) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((left - 0.5) * 10000.0);   // (100 ns units, relative)
        if (SetWaitableTimer(*timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(*timer, 1000);
    }
    while (NowMs() < targetMs) YieldProcessor();
}

struct ShowCall { const MmlGfxShow *show; int n; };
void CallShow(void *p) {
    auto *c = static_cast<ShowCall *>(p);
    c->n = g_showFn.fn(c->show, g_showFn.user);
}

// The back buffer to the mod and its pictures back: how many to show before the frame (0: the frame alone).
int ShowPrepare() {
    double t0 = NowMs();
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(g_dev9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return 0;
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE || !EnsureShow(d.Width, d.Height)) { SafeRelease(bb); return 0; }
    g_frameError.clear();
    g_frameLate = false;
    bool ok = Settled() && SUCCEEDED(g_dev9->StretchRect(bb, nullptr, g_showSurf9, nullptr, D3DTEXF_NONE)) && (GpuSync() || Wait9()) &&
              Hand(true, {g_showLink});
    SafeRelease(bb);
    if (!ok) { g_showLate++; return 0; }
    // the mod's pictures, its state as the passes have it (the frame's constants, samplers, full-screen shader)
    g_ctx->UpdateSubresource(g_frameCB, 0, nullptr, &g_k, 0, 0);
    MmlGfxFrame f{};
    f.size = sizeof f;
    f.device = g_dev11;
    f.context = g_ctx;
    f.generation = g_generation;
    f.k = &g_k;
    f.constants = g_frameCB;
    f.depth = d.Width != g_w || d.Height != g_h ? nullptr : g_upMade ? g_upDepthSRV : g_depthSRV;   // (the frame's at its size)
    f.color = g_showSRV;
    f.point = g_point;
    f.linear = g_linear;
    f.target = nullptr;
    f.full_screen = g_vsFull;
    f.width = d.Width;
    f.height = d.Height;
    memcpy(f.view, g_viewM, sizeof f.view);
    memcpy(f.proj, g_projM, sizeof f.proj);
    ID3D11ShaderResourceView *motion = g_motionReady && f.depth ? (g_upMade ? g_upMotionSRV : g_motionSRV) : nullptr;
    f.motion = motion;
    f.render_width = d.Width, f.render_height = d.Height;
    f.jitter[0] = g_jitNow[0], f.jitter[1] = g_jitNow[1];
    MmlGfxShow s{};
    s.size = sizeof s;
    s.frame = &f;
    s.max_extra = MML_GFX_SHOW_MAX;
    for (int i = 0; i < MML_GFX_SHOW_MAX; i++) s.extra[i] = g_extraRTV[i];
    s.interval_ms = g_interval;
    s.work_ms = g_work;
    s.shows = g_shows;
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView *srv[4] = {static_cast<ID3D11ShaderResourceView *>(f.depth), g_showSRV, nullptr, motion};
    ID3D11SamplerState *smp[2] = {g_point, g_linear};
    g_ctx->PSSetShaderResources(0, 4, srv);
    g_ctx->PSSetSamplers(0, 2, smp);
    g_ctx->PSSetConstantBuffers(0, 1, &g_frameCB);
    g_ctx->VSSetShader(g_vsFull, nullptr, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    ShowCall call{&s, 0};
    g_inPasses = true;   // (gfx_shared_texture and gfx_shared_fence answer inside it, as in a pass)
    ModCall(g_showFn.mod, CallShow, &call, "its showing (gfx_show)");
    g_inPasses = false;
    int n = std::clamp(call.n, 0, MML_GFX_SHOW_MAX);
    g_ctx->ClearState();   // the shared textures are Direct3D 9's again
    std::vector<gfxvk::Link *> back;
    for (int i = 0; i < n; i++) back.push_back(g_extraLink[i]);
    ok = GpuSync() || Wait11();
    if (ok && gfxvk::On() && (n || GpuSync())) {   // (with the fences, To9 also queues Direct3D 11's signal)
        std::string why;
        ok = gfxvk::To9(back.data(), n, &why);
        if (!ok) g_frameError = "the pictures' copy on DXVK's queue: " + why, g_frameLate = gfxvk::Busy();
    }
    if (!ok) { g_showLate++; return 0; }
    double ms = NowMs() - t0;
    g_showMs = g_showMs > 0 ? g_showMs * 0.95 + ms * 0.05 : ms;
    return n;
}

// ---- the showing from the bridge's own thread: the game's thread copies the frame and the pictures into a slot and
// goes on at once (the game's Present itself is left out); the thread shows them, spaced, through a swap chain of its
// own on the game's window. Direct3D 9 lets a swap chain present between another thread's BeginScene and EndScene as
// long as its back buffer is not the device's target: the game's back buffer never reaches this one. The device is
// made thread-safe for it (D3DCREATE_MULTITHREADED, hkCreateDevice8). Two slots: one shown, one waiting; the game
// waits only for the one waiting to be taken (it runs at most a frame ahead of what is seen). Its swap chain waits for
// the display's refresh, and the thread keeps a refresh between two things shown itself (DXVK's Present returns at
// once): what is shown never outruns the display. Not for a full-screen device (one swap chain alone may show there):
// the game's thread shows them then, as before.
struct ShowSlot { IDirect3DSurface9 *pic[MML_GFX_SHOW_MAX + 1] = {}; int n = 0; double interval = 0; };
IDirect3DSwapChain9 *g_showChain;
IDirect3DSurface9 *g_showChainBB;
ShowSlot g_slots[2];
UINT g_slotW, g_slotH;
std::mutex g_slotMx;
std::condition_variable g_slotCv;
int g_slotQueued = -1, g_slotShowing = -1;
bool g_presenterStop;
HANDLE g_presenter;                // (a Windows thread: a std::thread left joinable at the process's end terminates it)
HANDLE g_presenterTimer;
HRESULT g_presenterHr = S_OK;      // the thread's last Present (a lost device: the game's own Present shows it)
double g_refreshMs;                // the display's period (0: not known): two things are not shown closer than it
std::string g_threadWhy;           // why the showing is on the game's thread (gfx.state show.thread)
bool g_threadTried;

DWORD WINAPI Presenter(void *) {
    double lastEnd = 0, lastPresent = 0;
    for (;;) {
        int idx;
        {
            std::unique_lock<std::mutex> lk(g_slotMx);
            g_slotCv.wait(lk, [] { return g_presenterStop || g_slotQueued >= 0; });
            if (g_presenterStop) return 0;
            idx = g_slotShowing = g_slotQueued;
            g_slotQueued = -1;
        }
        g_slotCv.notify_all();
        const ShowSlot &s = g_slots[idx];
        double interval = std::clamp(s.interval > 0 ? s.interval : 16.7, 2.0, 100.0);
        double slot = interval / (s.n + 1);
        double t0 = std::max(NowMs(), lastEnd);
        for (int i = 0; i <= s.n; i++) {
            if (i) WaitUntil(t0 + slot * i, &g_presenterTimer);
            // (the display's period at least since the last: DXVK's Present does not wait for the refresh, its own
            // thread shows; the slots then hold the game at the refresh / (pictures + 1) as vsync does on Direct3D 9)
            if (g_refreshMs > 0 && lastPresent > 0) WaitUntil(lastPresent + g_refreshMs * 0.98, &g_presenterTimer);
            g_dev9->StretchRect(s.pic[i], nullptr, g_showChainBB, nullptr, D3DTEXF_NONE);
            double at = NowMs();
            if (g_lastShown > 0 && at - g_lastShown < 250) {
                double gap = at - g_lastShown, err = std::fabs(gap - slot);
                g_gap = g_gap > 0 ? g_gap * 0.98 + gap * 0.02 : gap;
                g_gapErr = g_gapErr > 0 ? g_gapErr * 0.98 + err * 0.02 : err;
            }
            g_lastShown = at;
            g_presenterHr = g_showChain->Present(nullptr, nullptr, nullptr, nullptr, 0);
            lastPresent = NowMs();
            if (i < s.n) g_showPictures++;
        }
        g_showFrames++;
        lastEnd = t0 + slot * s.n;   // (the next frame's pictures not before this one)
        {
            std::lock_guard<std::mutex> lk(g_slotMx);
            g_slotShowing = -1;
        }
        g_slotCv.notify_all();
    }
}

void ShowThreadStop() {
    if (g_presenter) {
        {
            std::lock_guard<std::mutex> lk(g_slotMx);
            g_presenterStop = true;
        }
        g_slotCv.notify_all();
        WaitForSingleObject(g_presenter, 5000);
        CloseHandle(g_presenter);
        g_presenter = nullptr;
    }
    g_presenterStop = false;
    g_slotQueued = g_slotShowing = -1;
    for (auto &s : g_slots) for (auto *&p : s.pic) SafeRelease(p);
    SafeRelease(g_showChainBB);
    SafeRelease(g_showChain);
    g_slotW = g_slotH = 0;
    g_threadTried = false;
    g_presenterHr = S_OK;
}

// The thread, its swap chain and its slots at the back buffer's size: false (and why) to show on the game's thread.
bool EnsureShowThread(UINT w, UINT h) {
    if (g_presenter && w == g_slotW && h == g_slotH) return true;
    if (g_threadTried && w == g_slotW && h == g_slotH) return false;
    ShowThreadStop();
    g_threadTried = true;
    g_slotW = w, g_slotH = h;
    auto no = [](const std::string &why) { g_threadWhy = why; HLOG("gfx: the showing on the game's thread: %s", why.c_str()); return false; };
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(g_dev9->GetCreationParameters(&cp)) || !(cp.BehaviorFlags & D3DCREATE_MULTITHREADED))
        return no("the device is not thread-safe (made before the loader, or without a mod with \"gfx\": true)");
    IDirect3DSwapChain9 *own = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    if (FAILED(g_dev9->GetSwapChain(0, &own)) || !own) return no("no swap chain of the game's");
    own->GetPresentParameters(&pp);
    own->Release();
    if (!pp.Windowed) return no("a full-screen device");
    D3DPRESENT_PARAMETERS mine{};
    mine.BackBufferWidth = w, mine.BackBufferHeight = h, mine.BackBufferFormat = pp.BackBufferFormat;
    mine.BackBufferCount = 1;
    mine.SwapEffect = D3DSWAPEFFECT_DISCARD;
    mine.hDeviceWindow = pp.hDeviceWindow ? pp.hDeviceWindow : cp.hFocusWindow;
    mine.Windowed = TRUE;
    // each Present waits for the display's refresh: what is shown never outruns it, and the slots (the game waits for
    // the one waiting to be taken) hold the game at the refresh / (pictures + 1)
    mine.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    D3DDISPLAYMODE dm{};
    g_refreshMs = SUCCEEDED(g_dev9->GetDisplayMode(0, &dm)) && dm.RefreshRate >= 24 ? 1000.0 / dm.RefreshRate : 0;
    HRESULT hr = g_dev9->CreateAdditionalSwapChain(&mine, &g_showChain);
    if (FAILED(hr) || !g_showChain) return no(Fmt("CreateAdditionalSwapChain failed (0x%08X)", static_cast<unsigned>(hr)));
    if (FAILED(g_showChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &g_showChainBB))) { ShowThreadStop(); g_threadTried = true; return no("its back buffer"); }
    for (auto &s : g_slots)
        for (auto *&p : s.pic)
            if (FAILED(g_dev9->CreateRenderTarget(w, h, pp.BackBufferFormat, D3DMULTISAMPLE_NONE, 0, FALSE, &p, nullptr))) {
                ShowThreadStop();
                g_threadTried = true, g_slotW = w, g_slotH = h;
                return no("its slots' surfaces");
            }
    g_presenter = CreateThread(nullptr, 0, Presenter, nullptr, 0, nullptr);
    if (!g_presenter) { ShowThreadStop(); g_threadTried = true, g_slotW = w, g_slotH = h; return no("its thread was not made"); }
    g_threadWhy.clear();
    HLOG("gfx: the showing from the bridge's own thread (%ux%u, a swap chain of its own)", w, h);
    return true;
}

// The game's thread: the frame (the back buffer, not yet shown) and its n pictures into the free slot, handed to the
// thread
HRESULT ShowQueue(int n) {
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(g_dev9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return D3D_OK;
    int idx;
    {
        std::unique_lock<std::mutex> lk(g_slotMx);
        g_slotCv.wait(lk, [] { return g_slotQueued < 0; });   // (the one waiting taken: the other slot is free)
        idx = g_slotShowing == 0 ? 1 : 0;
    }
    ShowSlot &s = g_slots[idx];
    g_quiet++;
    for (int i = 0; i < n; i++) g_dev9->StretchRect(g_extraSurf9[i], nullptr, s.pic[i], nullptr, D3DTEXF_NONE);
    g_dev9->StretchRect(bb, nullptr, s.pic[n], nullptr, D3DTEXF_NONE);
    g_quiet--;
    bb->Release();
    s.n = n, s.interval = g_interval;
    {
        std::lock_guard<std::mutex> lk(g_slotMx);
        g_slotQueued = idx;
    }
    g_slotCv.notify_all();
    return g_presenterHr == D3DERR_DEVICELOST ? D3DERR_DEVICELOST : D3D_OK;
}

// The pictures, then the frame, each copied into the back buffer and shown, spaced evenly over the frames' interval.
HRESULT ShowPictures(int n, const std::function<HRESULT()> &present) {
    double interval = std::clamp(g_interval > 0 ? g_interval : 16.7, 2.0, 100.0);
    double slot = interval / (n + 1);
    double t0 = NowMs();
    HRESULT hr = S_OK;
    for (int i = 0; i <= n; i++) {
        if (i) WaitUntil(t0 + slot * i, &g_showTimer);
        IDirect3DSurface9 *bb = nullptr;
        if (SUCCEEDED(g_dev9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            g_quiet++;
            g_dev9->StretchRect(i < n ? g_extraSurf9[i] : g_showSurf9, nullptr, bb, nullptr, D3DTEXF_NONE);
            g_quiet--;
            bb->Release();
        }
        double at = NowMs();
        if (g_lastShown > 0 && at - g_lastShown < 250) {   // (the spacing measured: each one shown against the one before)
            double gap = at - g_lastShown, err = std::fabs(gap - slot);
            g_gap = g_gap > 0 ? g_gap * 0.98 + gap * 0.02 : gap;
            g_gapErr = g_gapErr > 0 ? g_gapErr * 0.98 + err * 0.02 : err;
        }
        g_lastShown = at;
        hr = present();
        if (i < n) g_showPictures++;
    }
    g_showFrames++;
    return hr;
}
} // namespace

namespace {
// ---- the frame's times: Direct3D 9's GPU time of each frame (timestamps), the minute's line in the log
bool g_stampsOff;   // the device has no timestamp queries (made once, refused: not asked again)
void ReadStamps() {
    for (Stamp &s : g_stamps) {
        if (!s.pending) continue;
        BOOL disjoint = TRUE;
        UINT64 freq = 0, b = 0, e = 0;
        if (s.disjoint->GetData(&disjoint, sizeof disjoint, 0) != S_OK || s.freq->GetData(&freq, sizeof freq, 0) != S_OK ||
            s.begin->GetData(&b, sizeof b, 0) != S_OK || s.end->GetData(&e, sizeof e, 0) != S_OK)
            continue;
        s.pending = false;
        if (disjoint || !freq || e < b) continue;
        g_gpuFrame = 1000.0 * static_cast<double>(e - b) / static_cast<double>(freq);
        g_tm.gpuMs += g_gpuFrame, g_tm.gpuFrames++;
    }
}
void StampEnd() {
    if (!g_stampNow) return;
    Stamp &s = *g_stampNow;
    g_stampNow = nullptr;
    s.end->Issue(D3DISSUE_END);
    s.freq->Issue(D3DISSUE_END);
    s.disjoint->Issue(D3DISSUE_END);
    s.open = false;
    s.pending = true;
    g_stampNext = (g_stampNext + 1) % kStamps;
}

const char *SyncName() {
    return GpuSync() ? "the devices ordered on the GPU (DXVK, shared fences)" :
           gfxvk::On() ? "DXVK, the hand-overs waited for on the CPU" : "Direct3D 9Ex: the devices waited for on the CPU";
}

// Once a minute while the bridge runs: what a frame took, and of it the game thread's waits for the devices and its
// time in Present, the GPU's time of the frame and of the passes (a player's log says what holds the frame rate).
void TimingLog(double now) {
    if (!g_tmLoggedAt) { g_tmLoggedAt = now; g_tmLogged = g_tm; return; }
    if (now - g_tmLoggedAt < 60000) return;
    const Timing &a = g_tm, &b = g_tmLogged;
    const double n = static_cast<double>(a.frames - b.frames), secs = (now - g_tmLoggedAt) / 1000;
    const double gn = static_cast<double>(a.gpuFrames - b.gpuFrames), pn = static_cast<double>(a.passFrames - b.passFrames);
    if (n > 0 && g_frames && !g_failed) {
        std::string up;
        if (g_up.fn && ModAlive(g_up.mod))
            up = Fmt("; the world drawn at %ux%u for %s's upscaler (the frame %ux%u)", g_dw, g_dh, ModIdOf(g_up.mod), g_w, g_h);
        HLOG("gfx: the last minute: %.0f frames, %.1f a second; a frame %.2f ms: the game thread waited %.2f ms of it for the devices "
             "(Direct3D 9 %.2f, Direct3D 11 %.2f), %.2f handing over on DXVK's queue, %.2f in Present; the GPU: the frame %.2f ms "
             "(Direct3D 9, its first drawing to its Present), the passes %.2f%s; %s",
             n, n / secs, (a.frameMs - b.frameMs) / n, (a.wait9Ms - b.wait9Ms + a.wait11Ms - b.wait11Ms) / n, (a.wait9Ms - b.wait9Ms) / n,
             (a.wait11Ms - b.wait11Ms) / n, (a.handMs - b.handMs) / n, (a.presentMs - b.presentMs) / n,
             gn > 0 ? (a.gpuMs - b.gpuMs) / gn : 0.0, pn > 0 ? (a.passesMs - b.passesMs) / pn : 0.0, up.c_str(), SyncName());
    }
    g_tmLogged = g_tm;
    g_tmLoggedAt = now;
}
} // namespace

void GfxFrameBegin() {
    if (!g_dev9 || g_failed || g_stampsOff || g_stampNow) return;
    ReadStamps();
    Stamp &s = g_stamps[g_stampNext];
    if (s.pending) return;   // (the GPU that far behind: this frame goes untimed)
    if (!s.disjoint) {
        if (FAILED(g_dev9->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint)) ||
            FAILED(g_dev9->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq)) || FAILED(g_dev9->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.begin)) ||
            FAILED(g_dev9->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.end))) {
            SafeRelease(s.disjoint); SafeRelease(s.freq); SafeRelease(s.begin); SafeRelease(s.end);
            g_stampsOff = true;
            HWARN("gfx: Direct3D 9 has no timestamp queries: the frame's GPU time is not measured");
            return;
        }
    }
    s.disjoint->Issue(D3DISSUE_BEGIN);
    s.begin->Issue(D3DISSUE_END);
    s.open = true;
    g_stampNow = &s;
}

void GfxDrawnSize(float out[4]) {
    const UINT w = g_dw ? g_dw : g_w, h = g_dh ? g_dh : g_h;
    out[0] = static_cast<float>(w), out[1] = static_cast<float>(h);
    out[2] = w ? 1.0f / w : 0, out[3] = h ? 1.0f / h : 0;
}

int GfxUpscaleSet(MmlMod *mod, unsigned w, unsigned h, MmlGfxUpscaleFn fn, void *user) {
    if (!fn) {
        if (g_up.mod == mod) g_up = UpscaleAsk{};
        return 1;
    }
    if (g_up.fn && g_up.mod != mod && ModAlive(g_up.mod)) return 0;
    g_up = UpscaleAsk{mod, fn, user, w, h};
    return 1;
}

// The game's Present (screenshot.cpp's hook): the mod's pictures before the frame when one asked (gfx_show).
HRESULT GfxShow(void *dev8, const std::function<HRESULT()> &present) {
    double start = NowMs();
    if (g_showStart > 0) {
        double dt = start - g_showStart, work = start - g_showEnd;
        if (dt > 0 && dt < 500) {
            g_interval = g_interval > 0 ? g_interval * 0.9 + dt * 0.1 : dt;
            g_work = g_work > 0 ? g_work * 0.9 + work * 0.1 : work;
            g_tm.frames++, g_tm.frameMs += dt;
        }
    }
    g_showStart = start;
    if (dev8 == g_d3d8) StampEnd();
    auto timed = [&]() {   // (the game's Present: its time in it, the device's own wait for the GPU among it)
        double t0 = NowMs();
        HRESULT r = present();
        g_tm.presentMs += NowMs() - t0;
        return r;
    };
    int n = 0;
    const bool held = g_showFn.fn && ModAlive(g_showFn.mod) && !g_failed && g_dev11 && g_dev9 && dev8 == g_d3d8 && g_frames;
    if (held) {
        g_quiet++;
        n = ShowPrepare();
        g_quiet--;
    }
    HRESULT hr;
    if (held && (n > 0 || g_presenter) && g_showW && EnsureShowThread(g_showW, g_showH)) {
        double t0 = NowMs();
        hr = ShowQueue(n);   // (the thread shows them, a frame alone too while the showing is held: the game goes on)
        g_tm.presentMs += NowMs() - t0;   // (its wait for a free slot: the game held by the showing)
    } else {
        if (g_presenter) ShowThreadStop();   // (a frame without pictures: the game's own Present again)
        if (n <= 0) g_lastShown = 0;   // (a frame alone: the spacing is not measured across it)
        hr = n > 0 ? ShowPictures(n, timed) : timed();
    }
    g_showEnd = NowMs();
    g_shows++;
    if (gfxvk::On()) g_tm.handMs += gfxvk::TakeMs();
    if (dev8 == g_d3d8) TimingLog(g_showEnd);
    return hr;
}

int GfxShowSet(MmlMod *mod, MmlGfxShowFn fn, void *user) {
    if (!fn) {
        if (g_showFn.mod == mod) g_showFn = ShowFn{};
        return 1;
    }
    if (g_showFn.fn && g_showFn.mod != mod && ModAlive(g_showFn.mod)) return 0;
    g_showFn = ShowFn{mod, fn, user};
    return 1;
}

int GfxJitterSet(MmlMod *mod, int on, float x, float y) {
    if (!on) {
        if (g_jitter.mod == mod) g_jitter = JitterAsk{};
        return 1;
    }
    if (g_jitter.mod && g_jitter.mod != mod && ModAlive(g_jitter.mod)) return 0;
    g_jitter = JitterAsk{mod, {x, y}};
    return 1;
}

void GfxInit() {
    QueryPerformanceFrequency(&g_freq);
    void *old = nullptr;
    for (const char *m : {"LS3DF.dll", "Game.exe"}) {
        HMODULE mod = m[0] == 'G' ? GetModuleHandleA(nullptr) : GetModuleHandleA(m);
        if (mod && PatchImport(mod, "d3d8.dll", "Direct3DCreate8", reinterpret_cast<void *>(&hkDirect3DCreate8), &old)) {
            oDirect3DCreate8 = reinterpret_cast<Direct3DCreate8Fn>(old);
            return;
        }
    }
}

int GfxPassAdd(MmlMod *mod, int order, MmlGfxPassFn fn, void *user) {
    if (!fn || !ModAlive(mod)) return 0;
    if (g_passes.size() >= kTimerMarks - 2) { Log(MML_LOG_WARN, ModIdOf(mod), "gfx_pass: %zu passes already", g_passes.size()); return 0; }
    Pass p{mod, g_nextHandle++, order, ++g_seq, fn, user};
    g_passes.push_back(p);
    std::stable_sort(g_passes.begin(), g_passes.end(), [](const Pass &a, const Pass &b) {
        return a.order != b.order ? a.order < b.order : a.seq < b.seq;
    });
    if (!g_failed) g_state = g_dev11 ? "on" : "waiting for the mission's world";
    return p.handle;
}

bool GfxPassViews(MmlMod *mod, int handle, bool on) {
    for (Pass &p : g_passes)
        if (p.mod == mod && p.handle == handle) { p.inViews = on; return true; }
    return false;
}

void GfxPassRemove(MmlMod *mod, int handle) {
    g_passes.erase(std::remove_if(g_passes.begin(), g_passes.end(), [&](const Pass &p) {
        return p.mod == mod && (handle == 0 || p.handle == handle);
    }), g_passes.end());
    if (g_passes.empty() && !g_failed && g_dev11) g_state = "on, no pass (the device is kept)";
}
void GfxRemove(MmlMod *mod) {
    GfxPassRemove(mod, 0);
    MotionRemove(mod);
    GfxUiAsk(mod, MML_GFX_UI_OFF);
    for (auto &t : g_sideTex) if (t.mod == mod) ReleaseSideTex(t);
    g_sideTex.erase(std::remove_if(g_sideTex.begin(), g_sideTex.end(), [&](const SideTex &t) { return t.mod == mod; }), g_sideTex.end());
    for (auto &f : g_sideFences) if (f.mod == mod) SafeRelease(f.fence);
    g_sideFences.erase(std::remove_if(g_sideFences.begin(), g_sideFences.end(), [&](const SideFence &f) { return f.mod == mod; }),
                       g_sideFences.end());
    GfxShowSet(mod, nullptr, nullptr);
    GfxJitterSet(mod, 0, 0, 0);
    GfxUpscaleSet(mod, 0, 0, nullptr, nullptr);
}

// A 64-bit half's fence on this device (Direct3D 11.4), for its 32-bit half's pass: 1 there (out filled), 0 not (yet).
int GfxSharedFence(MmlMod *mod, const char *name, MmlSharedFence *out) {
    if (!name || !out || out->size < sizeof(MmlSharedFence) || !g_inPasses || !g_dev11) return 0;
    std::string id = ModIdOf(mod);
    auto it = std::find_if(g_sideFences.begin(), g_sideFences.end(), [&](const SideFence &f) { return f.mod == mod && f.name == name; });
    SidecarFenceInfo now{};
    if (!SidecarFence(id, name, &now, false)) {   // none (yet, or no more)
        if (it != g_sideFences.end()) { SafeRelease(it->fence); g_sideFences.erase(it); }
        return 0;
    }
    if (it == g_sideFences.end() || it->stamp != now.stamp || it->gen != g_generation) {
        if (it == g_sideFences.end()) it = g_sideFences.insert(g_sideFences.end(), SideFence{mod, name, 0, 0, nullptr, 0});
        SafeRelease(it->fence);
        SidecarFenceInfo mine{};
        HRESULT hr = E_FAIL;
        ID3D11Device5 *dev5 = nullptr;
        if (SidecarFence(id, name, &mine, true)) {
            hr = g_dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5));
            if (SUCCEEDED(hr)) hr = dev5->OpenSharedFence(mine.handle, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&it->fence));
            SafeRelease(dev5);
            CloseHandle(mine.handle);
        }
        it->stamp = mine.handle ? mine.stamp : now.stamp;
        it->gen = g_generation;
        it->generation = now.generation;
        if (FAILED(hr)) {
            Log(MML_LOG_WARN, id.c_str(), "gfx_shared_fence %s: Direct3D 11.4 did not open it (0x%08X)", name, static_cast<unsigned>(hr));
            SafeRelease(it->fence);
        }
    }
    if (!it->fence) return 0;
    out->fence = it->fence;
    out->generation = it->generation;
    return 1;
}

// A 64-bit half's texture on this device, for its 32-bit half's pass: 1 there (out filled), 0 not (yet).
int GfxSharedTexture(MmlMod *mod, const char *name, MmlSharedTexture *out) {
    if (!name || !out || out->size < sizeof(MmlSharedTexture) || !g_inPasses || !g_dev11) return 0;
    std::string id = ModIdOf(mod);
    auto it = std::find_if(g_sideTex.begin(), g_sideTex.end(), [&](const SideTex &t) { return t.mod == mod && t.name == name; });
    SidecarTex now{};
    if (!SidecarTexture(id, name, &now, false)) {   // none (yet, or no more)
        if (it != g_sideTex.end()) { ReleaseSideTex(*it); g_sideTex.erase(it); }
        return 0;
    }
    if (it == g_sideTex.end() || it->stamp != now.stamp || it->gen != g_generation) {
        if (it == g_sideTex.end()) it = g_sideTex.insert(g_sideTex.end(), SideTex{mod, name, 0, 0, nullptr, nullptr, {}});
        ReleaseSideTex(*it);
        SidecarTex mine{};
        HRESULT hr = E_FAIL;
        ID3D11Device1 *dev1 = nullptr;
        if (SidecarTexture(id, name, &mine, true)) {
            hr = g_dev11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1));
            if (SUCCEEDED(hr)) hr = dev1->OpenSharedResource1(mine.handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&it->tex));
            if (SUCCEEDED(hr)) hr = g_dev11->CreateShaderResourceView(it->tex, nullptr, &it->srv);
            SafeRelease(dev1);
            CloseHandle(mine.handle);
        }
        it->stamp = mine.handle ? mine.stamp : now.stamp;
        it->gen = g_generation;
        it->info = mine.handle ? mine : now;
        if (FAILED(hr)) {
            Log(MML_LOG_WARN, id.c_str(), "gfx_shared_texture %s: Direct3D 11 did not open it (0x%08X)", name, static_cast<unsigned>(hr));
            ReleaseSideTex(*it);
        }
    }
    if (!it->tex) return 0;
    out->texture = it->tex;
    out->srv = it->srv;
    out->width = it->info.w;
    out->height = it->info.h;
    out->format = it->info.format;
    out->generation = it->info.generation;
    return 1;
}

void GfxQuiet(bool on) { g_quiet += on ? 1 : -1; }

int GfxStateNow(std::string *why) {
    if (g_failed) {
        *why = g_state.compare(0, 5, "off: ") == 0 ? g_state.substr(5) : g_state;
        return 0;
    }
    return g_dev11 && g_frames ? 1 : -1;
}

void GfxUiAsk(MmlMod *mod, int mode) {
    g_uiAsk.erase(std::remove_if(g_uiAsk.begin(), g_uiAsk.end(), [&](const std::pair<MmlMod *, int> &a) { return a.first == mod; }),
                  g_uiAsk.end());
    if (mode == MML_GFX_UI_TAKE || mode == MML_GFX_UI_HIDE) g_uiAsk.push_back({mod, mode});
}

// The 2D pass begins (the scene's stage 2, inside the world's Render): into the shared target, when a mod asks.
bool GfxUiBegin() {
    g_uiMode = UiMode();
    if (g_uiMode == MML_GFX_UI_OFF || !g_swapped || !g_dev9 || g_failed || !g_w) return false;
    g_quiet++;
    bool ok = false;
    if (!g_ui9) {
        std::string why;
        if (MakePair(g_w, g_h, D3DFMT_A8R8G8B8, &g_ui9, &g_ui11, &g_uiLink, &why) &&
            SUCCEEDED(g_dev11->CreateShaderResourceView(g_ui11, nullptr, &g_uiSRV)))
            g_ui9->GetSurfaceLevel(0, &g_uiSurf9);
        if (!g_uiSurf9) HWARN("gfx: the 2D pass's target could not be made (%s): the HUD stays on the frame", why.c_str());
    }
    if (g_uiSurf9 && SUCCEEDED(g_dev9->GetRenderTarget(0, &g_uiGameRT)) && g_uiGameRT) {
        D3DSURFACE_DESC d{};
        g_uiGameRT->GetDesc(&d);
        if (d.Width == g_w && d.Height == g_h) {
            g_dev9->GetViewport(&g_uiViewport);
            g_dev9->SetRenderTarget(0, g_uiSurf9);
            g_dev9->SetViewport(&g_uiViewport);   // (SetRenderTarget set the whole target)
            g_dev9->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            // the game blends its 2D colour by its alpha: the alpha written as coverage (premultiplied over 0)
            g_dev9->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &g_uiSep);
            g_dev9->GetRenderState(D3DRS_SRCBLENDALPHA, &g_uiSrcA);
            g_dev9->GetRenderState(D3DRS_DESTBLENDALPHA, &g_uiDstA);
            g_dev9->GetRenderState(D3DRS_BLENDOPALPHA, &g_uiOpA);
            g_dev9->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            g_dev9->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
            g_dev9->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
            g_dev9->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
            ok = true;
        } else {
            SafeRelease(g_uiGameRT);
        }
    }
    g_quiet--;
    return ok;
}

// The 2D pass ended: the frame's target back; the 2D over the frame again unless a mod hides it.
void GfxUiEnd() {
    g_quiet++;
    g_dev9->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, g_uiSep);
    g_dev9->SetRenderState(D3DRS_SRCBLENDALPHA, g_uiSrcA);
    g_dev9->SetRenderState(D3DRS_DESTBLENDALPHA, g_uiDstA);
    g_dev9->SetRenderState(D3DRS_BLENDOPALPHA, g_uiOpA);
    g_dev9->SetRenderTarget(0, g_uiGameRT);
    g_dev9->SetViewport(&g_uiViewport);
    g_uiFrames++;
    if (g_uiMode == MML_GFX_UI_TAKE) {
        Saved9 saved;
        SaveAndNeutral9(saved);
        const float c[8] = {static_cast<float>(g_w), static_cast<float>(g_h), 1.0f / g_w, 1.0f / g_h, 0, 0, 0, 0};
        g_dev9->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_dev9->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_dev9->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_dev9->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        FullScreen9(g_psComposite, g_ui9, c);
        Restore9(saved);
    }
    SafeRelease(g_uiGameRT);
    g_quiet--;
}

void GfxBeforeWorld() {
    g_fogTallies = g_sunTallies = 0;   // a new frame: the tallies start over
    g_framePrims = g_litPrims = 0;
    g_upFrame = g_scaledNow = false;
    g_dw = g_w, g_dh = g_h;
    if (!Wants() || g_swapped || !Acquire(Device8())) return;
    IDirect3DSurface9 *ds = nullptr;
    if (FAILED(g_dev9->GetDepthStencilSurface(&ds)) || !ds) { g_skipped++; return; }
    D3DSURFACE_DESC d;
    ds->GetDesc(&d);
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
        SafeRelease(ds);
        Fail("the game's device is multisampled (" + g_msaa + "): a readable depth buffer cannot be; a mod with passes "
             "says \"gfx\": true in its mod.json and the game is started again (its device is then made without)");
        return;
    }
    if (!EnsureSize(d.Width, d.Height)) { SafeRelease(ds); return; }
    g_gameDS = ds;   // its reference kept until GfxAfterWorld
    g_lastGameDS = ds;
    g_swapped = true;   // (first: hkSetDepthStencil lets the bridge's own buffer be bound then)
    if (FAILED(g_dev9->SetDepthStencilSurface(g_intzSurf))) { g_swapped = false; SafeRelease(g_gameDS); g_skipped++; return; }
    // the upscaler's render size for this frame's world (its last ask): every viewport the game sets on the frame's
    // target scaled to it until stage 1 (the one set now taken too: LS3D sets its own only when its rectangle changes)
    g_upFrame = g_up.fn && ModAlive(g_up.mod);
    g_dw = g_w, g_dh = g_h;
    if (g_upFrame && g_up.w && g_up.h && g_stateHooked) g_dw = std::min<UINT>(g_up.w, g_w), g_dh = std::min<UINT>(g_up.h, g_h);
    g_scaledNow = g_dw != g_w || g_dh != g_h;
    {
        IDirect3DSurface9 *rt = nullptr;
        g_dev9->GetRenderTarget(0, &rt);
        g_rtFrame = IsFrameSize(rt);
        SafeRelease(rt);
        D3DVIEWPORT9 vp{};
        if (g_rtFrame && SUCCEEDED(g_dev9->GetViewport(&vp))) g_vpGame = vp;
        if (g_scaledNow && g_rtFrame) {
            D3DVIEWPORT9 sv = Scaled(vp);
            oSetViewport(g_dev9, &sv);
        }
    }
    if (g_scaledNow) LodBias(std::isnan(g_lodAsked) ? std::log2(static_cast<float>(g_dw) / static_cast<float>(g_w)) : g_lodAsked);
    // the world's jitter, one for the whole frame (the projection set now taken too: LS3D sets it only when it changes)
    const bool jit = g_jitter.mod && ModAlive(g_jitter.mod);
    g_jitNow[0] = jit ? g_jitter.want[0] : 0, g_jitNow[1] = jit ? g_jitter.want[1] : 0;
    g_jitWorld = g_jitNow[0] != 0 || g_jitNow[1] != 0;
    if (g_jitWorld) {
        D3DMATRIX p{};
        g_dev9->GetTransform(D3DTS_PROJECTION, &p);
        if (Perspective(p)) {
            g_projClean = p, g_projCleanHave = true;
            JitterInto(p, g_jitNow, 1);
            oSetTransform(g_dev9, D3DTS_PROJECTION, &p);
        }
    }
    g_motionThisFrame = false;
    if (MotionWanted() && EnsureMotion()) {   // (motion.cpp draws the moving objects again into the cleared target)
        std::string why;
        GeomEnsureHooks(&why);   // (a mod that asked before the game's device was there)
        GeomEnsureDip(&why);
        g_quiet++;
        IDirect3DSurface9 *rt = nullptr;   // (cleared as a target: ColorFill takes no float format everywhere)
        D3DVIEWPORT9 vp{};
        g_dev9->GetRenderTarget(0, &rt);
        g_dev9->GetViewport(&vp);
        g_dev9->SetRenderTarget(0, g_motionObjSurf9);
        g_dev9->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        g_dev9->SetRenderTarget(0, rt);
        g_dev9->SetViewport(&vp);
        SafeRelease(rt);
        g_quiet--;
        float projBefore[16];   // (the last camera through this frame's jitter: it cancels in the objects' vectors)
        memcpy(projBefore, g_projBefore, sizeof projBefore);
        if (g_jitWorld) JitterInto(projBefore, g_jitNow, 1);
        MotionFrameBegin(g_dev9, g_motionObjSurf9, g_cameraBefore ? g_viewBefore : nullptr, g_cameraBefore ? projBefore : nullptr);
        g_motionThisFrame = g_cameraBefore;
    }
}

void GfxTakeWorldLight() { if (g_stateHooked) TakeWorldLight(); }

// The world drawn: the game's own viewport back (what LS3D draws after it, its near pass and lens flares, and the 2D
// pass are at the frame's size, over the upscaled frame), and its own projection (the jitter is the world's alone).
void EndScaled() {
    if (!g_scaledNow) return;
    g_scaledNow = false;
    if (g_dev9 && g_rtFrame) oSetViewport(g_dev9, &g_vpGame);
    LodBias(0);
}
void EndJitter() {
    if (!g_jitWorld) return;
    g_jitWorld = false;
    if (g_dev9 && g_projCleanHave) oSetTransform(g_dev9, D3DTS_PROJECTION, &g_projClean);
}

void GfxStage1() {
    if (!g_swapped || !g_dev9) return;
    EndScaled();
    g_frameError.clear();
    g_frameLate = false;
    g_quiet++;
    bool ok = SafeStage1();
    g_quiet--;
    EndJitter();
    if (!ok && !g_failed) Fail("a fault while drawing the frame: " + g_faultWhere);
}

void GfxAfterWorld() {
    EndScaled();
    EndJitter();
    if (!g_swapped) return;
    g_swapped = false;   // (first: hkSetDepthStencil lets the game's own buffer be bound then)
    if (g_dev9 && g_gameDS) g_dev9->SetDepthStencilSurface(g_gameDS);
    SafeRelease(g_gameDS);
}

// ---- scene views: LS3D draws the scene into a view's targets (view.cpp), the passes get them
namespace {
ViewTargets *FindView(int handle) {
    for (auto &v : g_views) if (v->handle == handle) return v.get();
    return nullptr;
}
bool ViewTex(ViewTargets &v, D3DFORMAT f, IDirect3DTexture9 **t9, IDirect3DSurface9 **s9, ID3D11Texture2D **t11,
             ID3D11ShaderResourceView **srv, gfxvk::Link **link, const char *what, std::string *why) {
    std::string err;
    if (!MakePair(v.w, v.h, f, t9, t11, link, &err)) { *why = Fmt("its %s target: %s", what, err.c_str()); return false; }
    (*t9)->GetSurfaceLevel(0, s9);
    HRESULT hr = g_dev11->CreateShaderResourceView(*t11, nullptr, srv);
    if (FAILED(hr)) { *why = Fmt("its %s target's view (0x%08X)", what, static_cast<unsigned>(hr)); return false; }
    return true;
}
bool ViewTargetsFor(ViewTargets &v, UINT w, UINT h, std::string *why) {
    if (v.w == w && v.h == h && v.colorSRV && v.generation == g_generation) return true;
    ReleaseView(v);
    v.w = w;
    v.h = h;
    v.generation = g_generation;
    HRESULT hr = g_dev9->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, kFmtIntz, D3DPOOL_DEFAULT, &v.intz, nullptr);
    if (FAILED(hr)) { *why = Fmt("its depth buffer could not be made (0x%08X)", static_cast<unsigned>(hr)); ReleaseView(v); return false; }
    v.intz->GetSurfaceLevel(0, &v.intzSurf);
    if (!ViewTex(v, D3DFMT_A8R8G8B8, &v.color9, &v.colorSurf, &v.color11, &v.colorSRV, &v.colorLink, "colour", why) ||
        !ViewTex(v, D3DFMT_R32F, &v.depth9, &v.depthSurf, &v.depth11, &v.depthSRV, &v.depthLink, "linear depth", why)) {
        ReleaseView(v);
        return false;
    }
    HLOG("gfx: scene view %d: %ux%u (colour, a readable depth buffer, linear depth)", v.handle, w, h);
    return true;
}
} // namespace

bool GfxViewBegin(int handle, unsigned *pw, unsigned *ph, const float *clip, std::string *why) {
    unsigned w = *pw, h = *ph;
    if (g_viewNow) { *why = "another view is being drawn"; return false; }
    if (!Acquire(Device8())) { *why = "the graphics bridge is " + g_state; return false; }
    IDirect3DSurface9 *rt = nullptr;
    if (FAILED(g_dev9->GetRenderTarget(0, &rt)) || !rt) { *why = "the device has no render target"; return false; }
    if (!w || !h) {   // the frame's size: the target the frame is drawn into
        D3DSURFACE_DESC d{};
        rt->GetDesc(&d);
        w = d.Width;
        h = d.Height;
    }
    *pw = w;
    *ph = h;
    ViewTargets *v = FindView(handle);
    if (!v) {
        g_views.push_back(std::make_unique<ViewTargets>());
        v = g_views.back().get();
        v->handle = handle;
    }
    g_quiet++;
    if (!ViewTargetsFor(*v, w, h, why)) { g_quiet--; SafeRelease(rt); return false; }
    g_viewGameRT = rt;
    g_dev9->GetDepthStencilSurface(&g_viewGameDS);
    g_dev9->GetViewport(&g_viewGameVp);
    g_dev9->GetRenderState(D3DRS_CLIPPLANEENABLE, &g_viewClipOn);
    g_dev9->GetClipPlane(0, g_viewClip);
    g_dev9->SetRenderTarget(0, v->colorSurf);
    g_dev9->SetDepthStencilSurface(v->intzSurf);
    D3DVIEWPORT9 vp{0, 0, w, h, 0, 1};   // (LS3D's Render sets the same from the scene's rectangle)
    g_dev9->SetViewport(&vp);
    g_dev9->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    if (clip) {   // the fixed-function pipeline takes the plane in world space (not for the sky: g_viewClipWanted)
        g_dev9->SetClipPlane(0, clip);
        g_dev9->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
    } else {
        g_dev9->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    }
    g_viewClipWanted = clip != nullptr;
    memset(g_viewBackdrop, 0, sizeof g_viewBackdrop);
    g_quiet--;
    v->depthDone = false;
    g_viewNow = v;
    return true;
}

void GfxViewBackdrop(const float m[16]) { memcpy(g_viewBackdrop, m, sizeof g_viewBackdrop); }

namespace {
// The passes that asked (gfx_pass_views) over a view's picture, as over the frame's: its colour and depth to them,
// its effects target blended over its colour. Inside GfxViewStage1 (the bridge's states saved, its depth taken).
void ViewPasses(ViewTargets &v, const float c[8]) {
    bool any = false;
    for (const Pass &p : g_passes) any = any || (p.inViews && ModAlive(p.mod));
    if (!any || g_failed) return;
    if (!v.fx9) {
        std::string why;
        if (!MakePair(v.w, v.h, D3DFMT_A16B16G16R16F, &v.fx9, &v.fx11, &v.fxLink, &why) ||
            FAILED(g_dev11->CreateRenderTargetView(v.fx11, nullptr, &v.fxRTV))) {
            gfxvk::Free(v.fxLink);
            SafeRelease(v.fxRTV); SafeRelease(v.fx11); SafeRelease(v.fx9);
            v.passFails++;
            return;
        }
    }
    g_frameError.clear();
    g_frameLate = false;
    MmlGfxConstants k{};
    float vm[16], pm[16];
    bool ok = Settled() && (GpuSync() || Wait9()) && Hand(true, {v.colorLink, v.depthLink});   // (its colour and depth written)
    if (ok) {
        FillConstants(k, v.w, v.h, vm, pm);
        ok = RunPasses(k, {v.depthSRV, v.colorSRV, v.fxRTV, v.w, v.h, v.handle, vm, pm});
    }
    ok = ok && Hand(false, {v.fxLink});
    if (!ok) { v.passFails++; if (g_frameLate) g_lateFrames++; return; }
    g_dev9->SetRenderTarget(0, v.colorSurf);
    g_dev9->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev9->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_dev9->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_dev9->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    FullScreen9(g_psComposite, v.fx9, c);
    v.passRuns++;
}
} // namespace

// The view's world is drawn (its scene's stage 1; after the mods' VIEW_WORLD): its depth as linear view depth, the
// passes that asked over it. The near pass that follows clears the depth buffer, so it is taken here.
void GfxViewStage1() {
    ViewTargets *v = g_viewNow;
    if (!v || v->depthDone) return;
    g_quiet++;
    Saved9 saved;
    SaveAndNeutral9(saved);
    D3DMATRIX proj{};
    g_dev9->GetTransform(D3DTS_PROJECTION, &proj);
    const float c[8] = {static_cast<float>(v->w), static_cast<float>(v->h), 1.0f / v->w, 1.0f / v->h, proj._33, proj._43, kFarDepth, 0};
    g_dev9->SetRenderTarget(0, v->depthSurf);
    g_dev9->SetDepthStencilSurface(nullptr);
    FullScreen9(g_psDepth, v->intz, c);
    ViewPasses(*v, c);
    g_dev9->SetRenderTarget(0, v->colorSurf);
    g_dev9->SetDepthStencilSurface(v->intzSurf);
    Restore9(saved);
    g_quiet--;
    v->depthDone = true;
}

void GfxViewEnd(bool drawn, const float view[16], const float proj[16], const float eye[3]) {
    ViewTargets *v = g_viewNow;
    if (!v) return;
    g_viewNow = nullptr;
    g_viewClipWanted = false;
    g_quiet++;
    g_dev9->SetRenderState(D3DRS_CLIPPLANEENABLE, g_viewClipOn);
    g_dev9->SetClipPlane(0, g_viewClip);
    g_dev9->SetRenderTarget(0, g_viewGameRT);
    g_dev9->SetDepthStencilSurface(g_viewGameDS);
    g_dev9->SetViewport(&g_viewGameVp);
    g_quiet--;
    SafeRelease(g_viewGameRT);
    SafeRelease(g_viewGameDS);
    if (!drawn || !v->depthDone) { v->info.fresh = 0; return; }
    MmlGfxView &i = v->info;
    i.size = sizeof i;
    i.handle = v->handle;
    i.color = v->colorSRV;
    i.depth = v->depthSRV;
    i.color_texture = v->color11;
    i.width = v->w;
    i.height = v->h;
    memcpy(i.view, view, sizeof i.view);
    memcpy(i.proj, proj, sizeof i.proj);
    i.eye[0] = eye[0]; i.eye[1] = eye[1]; i.eye[2] = eye[2]; i.eye[3] = 0;
    i.frames++;
    i.fresh = 1;
}

void GfxViewsNewFrame() {
    for (auto &v : g_views) v->info.fresh = 0;
}

void GfxViewForget(int handle) {
    for (auto it = g_views.begin(); it != g_views.end(); ++it) {
        if ((*it)->handle != handle) continue;
        if (g_viewNow == it->get()) return;   // (not while it is drawn: the next frame's forget takes it)
        ReleaseView(**it);
        g_views.erase(it);
        return;
    }
}

bool GfxViewShow(int handle, const float rect[4]) {
    ViewTargets *v = FindView(handle);
    if (!v) {
        g_views.push_back(std::make_unique<ViewTargets>());
        v = g_views.back().get();
        v->handle = handle;
    }
    memcpy(v->show, rect, sizeof v->show);
    return true;
}

// The frame's stage 1 (render.cpp): the views asked for by view.show copied onto the frame (agents' eyes for a view).
void GfxViewsShowNow() {
    if (!g_dev9 || g_failed) return;
    IDirect3DSurface9 *rt = nullptr;
    for (auto &v : g_views) {
        if (v->show[2] <= 0 || !v->colorSurf || !v->info.frames) continue;
        if (!rt && (FAILED(g_dev9->GetRenderTarget(0, &rt)) || !rt)) return;
        D3DSURFACE_DESC d{};
        rt->GetDesc(&d);
        float sx = d.Width / 800.0f, sy = d.Height / 600.0f;
        RECT r{static_cast<LONG>(v->show[0] * sx), static_cast<LONG>(v->show[1] * sy), static_cast<LONG>((v->show[0] + v->show[2]) * sx),
               static_cast<LONG>((v->show[1] + v->show[3]) * sy)};
        r.right = std::min<LONG>(r.right, d.Width);
        r.bottom = std::min<LONG>(r.bottom, d.Height);
        if (r.right > r.left && r.bottom > r.top) g_dev9->StretchRect(v->colorSurf, nullptr, rt, &r, D3DTEXF_LINEAR);
    }
    SafeRelease(rt);
}

bool GfxMethod(const std::string &method, const std::string &params, std::string *reply, bool *ok) {
    if (method != "gfx.state" && method != "gfx.reset") return false;
    if (method == "gfx.reset") {
        if (g_failed) { g_failed = false; g_state = g_passes.empty() ? "idle (no pass registered)" : "waiting for the mission's world"; }
    }
    {   // (the frame's times since the last minute's line, or since {"timing_reset": true})
        json p = json::parse(params.empty() ? "{}" : params, nullptr, false);
        if (p.is_object() && p.value("timing_reset", false)) g_tmLogged = g_tm, g_tmLoggedAt = NowMs();
        if (p.is_object() && p.contains("mip_bias"))   // (a number: that bias under an upscaler; null: DLSS's own again)
            g_lodAsked = p["mip_bias"].is_number() ? p["mip_bias"].get<float>() : NAN;
    }
    auto r2 = [](double v) { return std::round(v * 1000) / 1000; };
    json passes = json::array();
    for (const Pass &p : g_passes)
        passes.push_back({{"mod", ModIdOf(p.mod)}, {"handle", p.handle}, {"order", p.order}, {"cpu_ms", r2(p.cpuMs)}, {"gpu_ms", r2(p.gpuMs)},
                          {"in_views", p.inViews}});
    json out = {{"state", g_state}, {"adapter", g_adapter}, {"size", {g_w, g_h}}, {"frames", g_frames}, {"generation", g_generation},
                {"passes", passes}, {"skipped", g_skipped}, {"not_our_depth", g_notOurDepth}, {"depth_redirects", g_depthRedirects},
                {"late", {{"frames", g_lateFrames}, {"runs", g_lateRuns}, {"now", g_lateRun}}},
                {"ms", {{"copy_9", r2(g_ms9)}, {"passes_11", r2(g_ms11)}, {"wait_11", r2(g_wait11)}, {"all", r2(g_msAll)},
                        {"gpu_passes", r2(g_gpuPasses)}}},
                {"following_state", g_stateHooked}, {"multisampling", g_msaa},
                {"ui", {{"mode", UiMode()}, {"frames", g_uiFrames}}},
                {"jitter", {{"mod", g_jitter.mod && ModAlive(g_jitter.mod) ? ModIdOf(g_jitter.mod) : std::string()},
                            {"now", {g_jitNow[0], g_jitNow[1]}}}},
                {"show", {{"mod", g_showFn.fn ? ModIdOf(g_showFn.mod) : std::string()}, {"shows", g_shows}, {"frames", g_showFrames},
                          {"pictures", g_showPictures}, {"late", g_showLate}, {"interval_ms", r2(g_interval)}, {"work_ms", r2(g_work)},
                          {"ms", r2(g_showMs)}, {"gap_ms", r2(g_gap)}, {"gap_error_ms", r2(g_gapErr)}, {"size", {g_showW, g_showH}},
                          {"thread", g_presenter ? std::string("on") : g_threadWhy},
                          {"thread_present", Fmt("0x%08X", static_cast<unsigned>(g_presenterHr))}, {"error", g_showError}}}};
    json views = json::array();
    for (auto &v : g_views)
        views.push_back({{"handle", v->handle}, {"size", {v->w, v->h}}, {"frames", v->info.frames}, {"fresh", v->info.fresh != 0},
                         {"pass_runs", v->passRuns}, {"pass_fails", v->passFails}});
    out["views"] = views;
    out["view_sky_sets"] = g_viewSkySets;
    out["gamma"] = GammaState();
    {
        const Timing &a = g_tm, &b = g_tmLogged;
        const double n = static_cast<double>(a.frames - b.frames), secs = g_tmLoggedAt ? (NowMs() - g_tmLoggedAt) / 1000 : 0;
        const double gn = static_cast<double>(a.gpuFrames - b.gpuFrames), pn = static_cast<double>(a.passFrames - b.passFrames);
        auto per = [&](double x) { return n > 0 ? r2(x / n) : 0.0; };
        out["timing"] = {{"frames", a.frames - b.frames}, {"seconds", r2(secs)}, {"fps", secs > 0 ? r2(n / secs) : 0.0},
                         {"frame_ms", per(a.frameMs - b.frameMs)}, {"wait9_ms", per(a.wait9Ms - b.wait9Ms)},
                         {"wait11_ms", per(a.wait11Ms - b.wait11Ms)}, {"hand_ms", per(a.handMs - b.handMs)},
                         {"present_ms", per(a.presentMs - b.presentMs)},
                         {"gpu_frame_ms", gn > 0 ? r2((a.gpuMs - b.gpuMs) / gn) : 0.0}, {"gpu_frame_last_ms", r2(g_gpuFrame)},
                         {"gpu_passes_ms", pn > 0 ? r2((a.passesMs - b.passesMs) / pn) : 0.0}, {"sync", SyncName()},
                         {"timestamps", !g_stampsOff}};
        out["upscale"] = {{"mod", g_up.fn && ModAlive(g_up.mod) ? ModIdOf(g_up.mod) : std::string()}, {"asked", {g_up.w, g_up.h}},
                          {"drawn", {g_dw, g_dh}}, {"frames_drawn", g_upDrawn}, {"frames_stretched", g_upStretched},
                          {"cpu_ms", r2(g_upCpuMs)}, {"gpu_ms", r2(g_upGpuMs)}, {"frame_at_its_size", g_upMade},
                          {"mip_bias", g_dw && g_w && g_dw != g_w ? r2(std::isnan(g_lodAsked) ? std::log2(static_cast<double>(g_dw) / g_w) : g_lodAsked) : 0.0}};
    }
    {
        json m = json::parse(MotionState(), nullptr, false);
        if (m.is_object()) {
            m["frames"] = g_motionFrames;
            m["ready"] = g_motionReady;
            if (!g_motionError.empty()) m["error"] = g_motionError;
            out["motion"] = m;
        }
    }
    out["d3d9"] = gfxvk::On() ? gfxvk::Describe() : g_dev9 ? "Direct3D 9Ex: textures shared by handle" : "";
    out["sun_shadow"] = {{"bound", g_sunSRV != nullptr && g_k.sun_shadow_info[0] > 0.5f}, {"size", g_k.sun_shadow_info[3]}};
    if (!g_sunError.empty()) out["sun_shadow"]["error"] = g_sunError;
    if (g_frames) {
        const MmlGfxConstants &k = g_k;
        out["world"] = {{"eye", {k.eye[0], k.eye[1], k.eye[2]}}, {"sun", {k.sun[0], k.sun[1], k.sun[2]}}, {"sun_from_game", k.sun[3] > 0.5f},
                        {"sun_color", {k.sun_color[0], k.sun_color[1], k.sun_color[2]}}, {"sun_from", g_sunFrom},
                        {"ambient", {k.ambient[0], k.ambient[1], k.ambient[2]}}, {"ambient_from_scene", k.ambient[3] > 0.5f},
                        {"fog", {{"on", k.fog_color[3] > 0.5f}, {"mode", k.fog[3]}, {"start", k.fog[0]}, {"end", k.fog[1]},
                                 {"density", k.fog[2]}, {"color", {k.fog_color[0], k.fog_color[1], k.fog_color[2]}}}}};
    }
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
