// dlss_nr: NVIDIA DLSS over Mafia's own frame, a mod in two halves (docs/MODDING.md "64-bit halves"): DLSS 5 Neural
// Rendering ("3D-guided neural rendering": NVIDIA's model that adds lighting and material detail to the picture the
// engine drew; a GeForce RTX 50) and DLSS Super Resolution (its modes DLAA, Quality, Balanced, Performance, Ultra
// Performance; a GeForce RTX 20 or newer). They run only on 64 bits, through Vulkan: the 64-bit half (dlss_nr64.cpp)
// runs them; this 32-bit half gives them the frame in its graphics pass and puts their picture on the screen, under
// the HUD.
//
// What DLSS is given: the frame where the world is drawn and the HUD is not yet (in its gamma), its depth, each pixel's
// motion vector (the bridge's: the camera's and the moving cars' and people's), and DLSS-NR's control mask (how much it
// may change each pixel: the sky's share a setting). With Super Resolution the inputs are at the render size of its
// mode, each render pixel the frame's pixel under a jittered point (a Halton sequence: DLSS gathers the sub-pixel
// samples over the frames); DLSS-NR runs at that size, then DLSS SR brings its picture to the frame's: DLSS-NR's cost
// is the render size's. The settings are NVIDIA's own, on the game's Options > Mods page; a key turns it on and off.
//
//   call dlss_nr.state  -> {"on", "stage", "message", "sr", "upscale", "render", "drawn", "late", "gpu_ms", ...}
//   call dlss_nr.toggle {"on": true}   (no "on": switched)        F12 (the setting "hotkey")
//
// The frames go round two slots (dlss_nr.h): in the pass, the picture of the frame before is drawn (this device's GPU
// waits for the 64-bit half's "done" of it: the CPU does not), then this frame's inputs are drawn into its slot, the
// shared fence "ready" signalled and "view" written; the 64-bit half queues its DLSS on them and answers at once. So
// the game draws frame N + 1 while DLSS works on N: the picture shown is a frame behind the game's (the setting
// "pipeline" off: this frame's, waited for, as the first version did).
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>   // (ID3D11DeviceContext4's Signal and Wait on the shared fences)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include "json.hpp"
#include "mml/mml.h"
#include "dlss_nr.h"
#include "shaders/PsPrepare.h"
#include "shaders/PsComposite.h"
#include "shaders/PsDebug.h"
#include "shaders/PsCopyIn.h"
#include "shaders/PsCopyScene.h"
#include "shaders/PsBlend.h"
#include "shaders/PsMvecIn.h"

using nlohmann::json;

namespace {
const MmlHost *H;
MmlMod *M;
MmlChannel *g_viewCh, *g_doneCh, *g_statusCh, *g_fgViewCh, *g_fgDoneCh;
double g_freqMs;
uint64_t g_frame;

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double Ms(int64_t a, int64_t b) { return (b - a) / g_freqMs; }

// ---- the settings (config.json's defaults; the Options' Mods page or config.set): read at load and when they change
struct Settings {
    bool on = true;
    std::string hotkey = "F12";
    bool nr = true;          // DLSS 5 NR (an RTX 50); off: DLSS SR alone
    uint32_t upscale = dnr::kUpQuality;
    bool pipeline = true;    // the picture a frame behind, DLSS working beside the game (off: each frame waits for it)
    float intensity = 1, tone = 0.3f, structure = 0.7f, skin = 0.5f, sky = 1;
    uint32_t model = 0, preset = 2, srPreset = 0;
    bool autoMask = true, split = false, messages = true;
    float splitAt = 0.5f;
    float jitterSign = -1;   // the jitter as DLSS takes it: the sample's offset times this (measured: see config.json)
    uint32_t debug = 0;      // 0 off, 1 motion vectors, 2 depth, 3 mask, 4 the round trip (NR bypassed: the frame through
                             // the 64-bit half untouched, or through DLSS SR alone), 5 the motion vectors checked (the
                             // frame before moved by them, against this one), 6 the same without them
    uint32_t waitMs = 250;
    uint32_t fg = 0;         // frame generation: the frames made between two (0 off, 1 2x, 2 3x, 3 4x)
    bool fgStandIn = false;  // the showing tested without DLSS-FG: the frames between two blended (any GPU)
    float mvScale = 1;       // the motion vectors scaled (a test of their size: tools/pass_test.py)
} S;

float Number(const char *key, double def, float lo, float hi) { return std::clamp(float(H->config_number(M, key, def)), lo, hi); }

void KeyToggle(void *);
void UpdateShow();
void ReadSettings() {
    Settings s;
    s.on = Number("on", 1, 0, 1) >= 0.5f;
    s.hotkey = H->config_string(M, "hotkey", "F12");
    s.nr = Number("nr", 1, 0, 1) >= 0.5f;
    s.upscale = uint32_t(std::lround(Number("upscale", dnr::kUpQuality, 0, dnr::kUpModes - 1)));
    s.pipeline = Number("pipeline", 1, 0, 1) >= 0.5f;
    s.intensity = Number("intensity", 1, 0, 1);
    s.tone = Number("tone", 0.3, 0, 1);
    s.structure = Number("structure", 0.7, 0, 1);
    s.model = uint32_t(std::lround(Number("model", 0, 0, 2)));
    s.autoMask = Number("auto_mask", 1, 0, 1) >= 0.5f;
    s.skin = Number("skin", 0.5, 0, 1);
    s.sky = Number("sky", 1, 0, 1);
    s.preset = uint32_t(std::lround(Number("preset", 2, 0, 7)));
    s.srPreset = uint32_t(std::lround(Number("sr_preset", 0, 0, 15)));
    s.split = Number("split", 0, 0, 1) >= 0.5f;
    s.splitAt = Number("split_at", 0.5, 0.05f, 0.95f);
    s.messages = Number("messages", 1, 0, 1) >= 0.5f;
    s.jitterSign = Number("jitter_sign", -1, -1, 1) < 0 ? -1.f : 1.f;
    s.debug = uint32_t(std::lround(Number("debug", 0, 0, 7)));
    s.waitMs = uint32_t(std::lround(Number("wait_ms", 250, 10, 2000)));
    s.fg = uint32_t(std::lround(Number("frame_gen", 0, 0, dnr::kFgMax)));
    s.fgStandIn = Number("fg_stand_in", 0, 0, 1) >= 0.5f;
    s.mvScale = Number("mv_scale", 1, -4, 4);
    S = s;
    H->hotkey(M, S.hotkey.c_str(), KeyToggle, nullptr);   // (the same function again: the combination replaced)
    UpdateShow();
}

const char *UpscaleName(uint32_t u) {
    static const char *const n[] = {"off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
    return u < dnr::kUpModes ? n[u] : "?";
}

// ---- what the 64-bit half said
dnr::Status g_status{};
uint64_t g_statusVersion;
uint32_t g_toldKey = UINT32_MAX;   // what was told on the HUD last (the stage, SR's availability, the modes)

void Tell(const char *text, uint32_t argb = 0xFFFFFFFF) {
    if (S.messages && H->mission_state() != MML_MISSION_NONE) H->hud_console(M, text, argb);
}

void ReadStatus() {
    uint64_t v = 0;
    dnr::Status s;
    if (H->channel_read(g_statusCh, &s, sizeof s, &v) == sizeof s && v != g_statusVersion) {
        bool srChanged = strcmp(s.srMessage, g_status.srMessage) != 0;
        g_statusVersion = v;
        g_status = s;
        H->log(M, s.stage == dnr::kReady || s.stage == dnr::kStarting ? MML_LOG_INFO : MML_LOG_WARN, "%s", s.message);
        if (srChanged && s.srMessage[0]) H->log(M, s.srAvailable ? MML_LOG_INFO : MML_LOG_WARN, "%s", s.srMessage);
    }
}

// What runs here, from the settings and what NGX said: DLSS-NR (an RTX 50 with its DLL), DLSS SR (an RTX with its DLL).
bool NrUsable() { return g_status.stage == dnr::kReady; }
uint32_t UpscaleUsable() { return g_status.srAvailable ? S.upscale : dnr::kUpOff; }

// What the player is told once NGX has spoken, and again whenever DLSS is switched on or its modes change.
void TellStatus(bool always) {
    if (g_status.stage == dnr::kStarting) return;
    uint32_t key = g_status.stage | g_status.srAvailable << 8 | (S.on ? 1u : 0u) << 9 | (S.nr ? 1u : 0u) << 10 | S.upscale << 11;
    if (!always && g_toldKey == key) return;
    g_toldKey = key;
    char b[64];   // (the console's line: 63 characters; the whole sentences are in the log and in dlss_nr.state)
    if (!S.on) return Tell("DLSS: off", 0xFF76B900);
    bool nr = S.nr && NrUsable();
    uint32_t up = UpscaleUsable();
    if (nr || up != dnr::kUpOff) {
        snprintf(b, sizeof b, "DLSS:%s%s%s%s", nr ? " 5 NR" : "", nr && up ? " +" : "", up ? " SR " : "", up ? UpscaleName(up) : "");
        Tell(b, 0xFF76B900);
    }
    if (S.nr && !nr) {   // (asked for and not here: why)
        switch (g_status.stage) {
        case dnr::kUnsupported: snprintf(b, sizeof b, "DLSS 5 NR: needs a GeForce RTX 50 GPU"); break;
        case dnr::kOtherGpu: snprintf(b, sizeof b, "DLSS: the game is not on the NVIDIA GPU (see the log)"); break;
        case dnr::kNoDll: snprintf(b, sizeof b, "DLSS 5 NR: nvngx_dlssnr.dll missing (see the log)"); break;
        case dnr::kOldDriver: snprintf(b, sizeof b, "DLSS 5 NR: update the NVIDIA driver (%u.%u+)", g_status.minDriver[0], g_status.minDriver[1]); break;
        default: snprintf(b, sizeof b, "DLSS 5 NR: not available (see the log)"); break;
        }
        Tell(b, 0xFFFF8040);
    }
    if (S.upscale != dnr::kUpOff && !up && g_status.stage != dnr::kOtherGpu) Tell("DLSS SR: not available (see the log)", 0xFFFF8040);
}

// The graphics bridge off (a game device it cannot share with): the pass never runs, so the player is told here why,
// once a mission and at F12 (a player's game once showed every setting on and nothing happening).
std::string g_bridgeOff;    // the reason, while off
bool g_bridgeTold;
void CheckBridge(bool always) {
    if (!MML_HAS(H, gfx_state)) return;
    char why[256];
    if (H->gfx_state(M, why, sizeof why) != 0) { g_bridgeOff.clear(); return; }
    if (!always && g_bridgeTold && g_bridgeOff == why) return;
    if (g_bridgeOff != why) H->log(M, MML_LOG_WARN, "the graphics bridge is off, so DLSS gets no frame: %s", why);
    g_bridgeOff = why;
    g_bridgeTold = true;
    Tell("DLSS: the graphics bridge is off (see the log)", 0xFFFF8040);
}

// ---- the measures
struct Stats {
    uint64_t frames, drawn, late, none, noTextures, silent, resets, cuts;
    double rtSum, rtMax, workSum, gpuSum, gpuNrSum, gpuSrSum, prepSum;
} T;

// What the frames did, into the log once a minute in a mission and at its close (a player's log then says it: how
// many frames had DLSS's picture, how many went late or without one, the history's resets, the times).
Stats g_logged{};
double g_windowRtMax;
int64_t g_loggedQpc;
uint32_t g_renderW, g_renderH, g_outW, g_outH;   // the sizes of the frame drawn last
void LogStats(const char *when) {
    const Stats &a = T, &b = g_logged;
    uint64_t frames = a.frames - b.frames, drawn = a.drawn - b.drawn;
    if (!frames) return;
    double n = drawn ? double(drawn) : 1;
    H->log(M, MML_LOG_INFO,
           "%s: %llu frames, %llu with DLSS's picture (%s%s, %ux%u to %ux%u, %s), %llu late (over %u ms), %llu without its picture, "
           "%llu waiting for the textures; the history reset %llu times (%llu at a camera cut); the answer %.2f ms (at most %.2f), "
           "on the GPU %.2f ms (NR %.2f, SR %.2f), the pass's own %.2f ms",
           when, (unsigned long long)frames, (unsigned long long)drawn, S.nr && NrUsable() ? "NR + " : "", UpscaleName(UpscaleUsable()),
           g_renderW, g_renderH, g_outW, g_outH, S.pipeline ? "pipelined" : "waited for", (unsigned long long)(a.late - b.late), S.waitMs,
           (unsigned long long)(a.none - b.none), (unsigned long long)(a.noTextures - b.noTextures),
           (unsigned long long)(a.resets - b.resets), (unsigned long long)(a.cuts - b.cuts), (a.rtSum - b.rtSum) / n, g_windowRtMax,
           (a.gpuSum - b.gpuSum) / n, (a.gpuNrSum - b.gpuNrSum) / n, (a.gpuSrSum - b.gpuSrSum) / n,
           (a.prepSum - b.prepSum) / double(frames));
    g_logged = T;
    g_windowRtMax = 0;
}

// ---- drawing (the bridge's Direct3D 11 device)
template <class X> void SafeRelease(X *&p) {
    if (p) p->Release(), p = nullptr;
}
struct Consts {   // dlss_nr.hlsl's DlssNr (b1)
    float prevEye[4], prevRight[4], prevUp[4], prevFwd[4], prevProj[4];
    float opts[4];
    float opts2[4];   // x: 1 when the bridge's motion vectors are bound (t3: the objects' motion too)
    float render[4];  // xy: the frame's size / the render size, zw: the inputs' sample offset (the jitter)
};
struct Gfx {
    unsigned generation = 0;
    ID3D11PixelShader *prepare = nullptr, *composite = nullptr, *debug = nullptr, *copyIn = nullptr, *copyScene = nullptr, *blend = nullptr;
    ID3D11PixelShader *mvecIn = nullptr;
    ID3D11Texture2D *fgColor = nullptr;      // DLSS-FG's input (the half's), its view of ours
    uint32_t fgColorGen = 0;
    ID3D11RenderTargetView *fgColorRtv = nullptr;
    ID3D11Texture2D *prevShow = nullptr;     // the stand-in's frame before (the back buffer as it was shown)
    ID3D11ShaderResourceView *prevShowSrv = nullptr;
    ID3D11Buffer *cb = nullptr;
    ID3D11DeviceContext4 *c4 = nullptr;
    ID3D11Texture2D *inTex[dnr::kSlots][4] = {};
    uint32_t inGen[dnr::kSlots][4] = {};
    ID3D11RenderTargetView *inRtv[dnr::kSlots][4] = {};
    ID3D11Texture2D *prev = nullptr;          // the frame before (the debug views' check of the motion vectors)
    ID3D11ShaderResourceView *prevSrv = nullptr;
    void FreeIn() {
        for (auto &slot : inRtv)
            for (auto *&r : slot) SafeRelease(r);
        memset(inTex, 0, sizeof inTex), memset(inGen, 0, sizeof inGen);
    }
    void Free() {
        FreeIn(), SafeRelease(prepare), SafeRelease(composite), SafeRelease(debug), SafeRelease(cb), SafeRelease(c4);
        SafeRelease(copyIn), SafeRelease(copyScene), SafeRelease(blend), SafeRelease(mvecIn);
        SafeRelease(prevSrv), SafeRelease(prev), SafeRelease(fgColorRtv), SafeRelease(prevShowSrv), SafeRelease(prevShow);
        fgColor = nullptr, fgColorGen = 0;
    }
} G;

bool Make(ID3D11Device *d, ID3D11DeviceContext *c) {
    G.Free();
    D3D11_BUFFER_DESC bd{sizeof(Consts), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    return SUCCEEDED(d->CreatePixelShader(kPsPrepare, sizeof kPsPrepare, nullptr, &G.prepare)) &&
           SUCCEEDED(d->CreatePixelShader(kPsComposite, sizeof kPsComposite, nullptr, &G.composite)) &&
           SUCCEEDED(d->CreatePixelShader(kPsDebug, sizeof kPsDebug, nullptr, &G.debug)) &&
           SUCCEEDED(d->CreatePixelShader(kPsCopyIn, sizeof kPsCopyIn, nullptr, &G.copyIn)) &&
           SUCCEEDED(d->CreatePixelShader(kPsCopyScene, sizeof kPsCopyScene, nullptr, &G.copyScene)) &&
           SUCCEEDED(d->CreatePixelShader(kPsBlend, sizeof kPsBlend, nullptr, &G.blend)) &&
           SUCCEEDED(d->CreatePixelShader(kPsMvecIn, sizeof kPsMvecIn, nullptr, &G.mvecIn)) &&
           SUCCEEDED(d->CreateBuffer(&bd, nullptr, &G.cb)) &&
           SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&G.c4)));
}

void FullScreen(ID3D11DeviceContext *c, const MmlGfxFrame *f, ID3D11PixelShader *ps) {
    c->VSSetShader(static_cast<ID3D11VertexShader *>(f->full_screen), nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->Draw(3, 0);
}

// The camera of the frame before (the motion vectors), and when DLSS's histories are dropped: the first frame, a new
// size or mode, a camera cut (a cutscene's new shot, a teleport: more than 4 m or 30 degrees in one frame).
struct Camera {
    bool have = false;
    float eye[4], right[4], up[4], fwd[4], proj[4];
    float viewM[16], projM[16];   // its matrices (row vectors: frame generation's camera)
    unsigned width = 0, height = 0;
    uint32_t key = UINT32_MAX;   // the model, the presets, the modes
} g_prev;
bool g_resetNext = true;
bool g_bridgeMotion;   // this frame's MmlGfxFrame has the bridge's motion vectors (t3)

uint32_t ModeKey() { return S.model | S.preset << 4 | S.srPreset << 8 | UpscaleUsable() << 16 | (S.nr ? 1u : 0u) << 20 | S.debug << 24; }

bool Cut(const MmlGfxConstants *k, const MmlGfxFrame *f) {
    if (!g_prev.have || g_prev.width != f->width || g_prev.height != f->height || g_prev.key != ModeKey()) return true;
    float dx = k->eye[0] - g_prev.eye[0], dy = k->eye[1] - g_prev.eye[1], dz = k->eye[2] - g_prev.eye[2];
    float turn = k->fwd[0] * g_prev.fwd[0] + k->fwd[1] * g_prev.fwd[1] + k->fwd[2] * g_prev.fwd[2];
    return dx * dx + dy * dy + dz * dz > 16 || turn < 0.866f;
}

void Keep(const MmlGfxConstants *k, const MmlGfxFrame *f) {
    memcpy(g_prev.eye, k->eye, sizeof g_prev.eye);
    memcpy(g_prev.right, k->right, sizeof g_prev.right);
    memcpy(g_prev.up, k->up, sizeof g_prev.up);
    memcpy(g_prev.fwd, k->fwd, sizeof g_prev.fwd);
    memcpy(g_prev.proj, k->proj, sizeof g_prev.proj);
    memcpy(g_prev.viewM, f->view, sizeof g_prev.viewM);
    memcpy(g_prev.projM, f->proj, sizeof g_prev.projM);
    g_prev.width = f->width, g_prev.height = f->height;
    g_prev.key = ModeKey();
    g_prev.have = true;
}

void Constants(ID3D11DeviceContext *c, const MmlGfxConstants *k, bool havePrev, float split, const float render[4] = nullptr) {
    Consts x{};
    const Camera &p = g_prev;
    memcpy(x.prevEye, havePrev ? p.eye : k->eye, sizeof x.prevEye);
    memcpy(x.prevRight, havePrev ? p.right : k->right, sizeof x.prevRight);
    memcpy(x.prevUp, havePrev ? p.up : k->up, sizeof x.prevUp);
    memcpy(x.prevFwd, havePrev ? p.fwd : k->fwd, sizeof x.prevFwd);
    memcpy(x.prevProj, havePrev ? p.proj : k->proj, sizeof x.prevProj);
    x.opts[0] = S.sky, x.opts[1] = havePrev ? 1.f : 0.f, x.opts[2] = split, x.opts[3] = float(S.debug);
    x.opts2[0] = g_bridgeMotion ? 1.f : 0.f;
    x.opts2[1] = S.mvScale;
    const float one[4] = {1, 1, 0, 0};
    memcpy(x.render, render ? render : one, sizeof x.render);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(G.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &x, sizeof x);
        c->Unmap(G.cb, 0);
    }
    ID3D11Buffer *b[2] = {static_cast<ID3D11Buffer *>(nullptr), G.cb};
    c->PSGetConstantBuffers(0, 1, &b[0]);   // (b0: the bridge's, kept)
    c->PSSetConstantBuffers(0, 2, b);
    if (b[0]) b[0]->Release();
}

// The jitter: a Halton (2, 3) sequence, as DLSS's guide asks, over 8 times the pixels' scale squared phases (8 at
// least: DLAA's, a render pixel a frame pixel).
float Halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    for (; i; i /= base) f /= float(base), r += f * float(i % base);
    return r;
}
void Jitter(uint64_t n, float scale, float out[2]) {
    uint32_t phases = std::max<uint32_t>(8, uint32_t(std::ceil(8 * scale * scale)));
    uint32_t i = uint32_t(n % phases) + 1;
    out[0] = Halton(i, 2) - 0.5f, out[1] = Halton(i, 3) - 0.5f;
}

// Each frame's sample, j render pixels from the render pixel's centre, comes from two places: the frame's whole pixels
// of it from where PsPrepare samples the frame (jr), the rest, under half a frame's pixel, from the game's own
// projection offset (gfx_jitter: jg, the world drawn there). A point of the frame's picture at p shows the scene at
// p - jg, so the render pixel's sample is its centre + jr - jg / scale. Without the game's offset (DLAA: a render
// pixel is a frame pixel) DLSS would have no sub-pixel sample at all: no anti-aliasing, the thin wires broken as the
// game draws them. jg is asked a frame ahead: MmlGfxFrame's jitter says what the frame was drawn with.
struct JitAsk { float jg[2], jr[2]; bool set; } g_jitAsk;
uint64_t g_jitN;
bool g_jitHeld, g_jitUsed;
void JitterRelease() {
    if (g_jitHeld && MML_HAS(H, gfx_jitter)) H->gfx_jitter(M, 0, 0, 0);
    g_jitHeld = false, g_jitAsk = JitAsk{};
}
// this frame's sampling offset and the sample DLSS is told of (render pixels); the next frame's asked
void JitterFrame(const MmlGfxFrame *f, uint32_t rw, float jr[2], float j[2]) {
    g_jitUsed = true;
    const float scale = rw ? float(f->width) / float(rw) : 1.f;
    float jg[2] = {0, 0};
    if (MML_GFX_HAS(f, jitter)) jg[0] = f->jitter[0], jg[1] = f->jitter[1];
    const bool paired = g_jitAsk.set && jg[0] == g_jitAsk.jg[0] && jg[1] == g_jitAsk.jg[1];
    jr[0] = paired ? g_jitAsk.jr[0] : 0, jr[1] = paired ? g_jitAsk.jr[1] : 0;
    j[0] = jr[0] - jg[0] / scale, j[1] = jr[1] - jg[1] / scale;
    float t[2];
    Jitter(++g_jitN, scale, t);
    JitAsk a{};
    a.set = true;
    for (int i = 0; i < 2; i++) {
        float whole = std::round(t[i] * scale);
        a.jg[i] = whole - t[i] * scale, a.jr[i] = whole / scale;
    }
    g_jitHeld = MML_HAS(H, gfx_jitter) && MML_GFX_HAS(f, jitter) && H->gfx_jitter(M, 1, a.jg[0], a.jg[1]);
    if (!g_jitHeld) {   // (a loader without it, or another mod holds it: the frame's pixels alone, above DLAA)
        a.jg[0] = a.jg[1] = 0;
        a.jr[0] = scale > 1 ? t[0] : 0, a.jr[1] = scale > 1 ? t[1] : 0;
    }
    g_jitAsk = a;
}

// ---- the ring (dlss_nr.h): what each slot's next writer must wait for, and the view answered last
struct Ring {
    uint32_t readyGen = 0, doneGen = 0;   // the shared fences' generations it counts on
    uint64_t slotDone[dnr::kSlots] = {};  // the "done" value a slot's inputs may be written after (0: none to wait for)
    uint64_t lastSent = 0;                // the last view written (prepared or an ask)
    bool lastPrepared = false;
    uint32_t lastSlot = 0;
} g_ring;

dnr::Done g_done{};
uint64_t g_doneVersion;
int64_t g_doneSeen;

// Each slot's frame as frame generation needs it: its inputs' size and generation, its camera and the one before
// (the frame whose picture the showing finds in the back buffer is a slot's: the frame before's when pipelined).
struct SlotCam {
    uint64_t frame = 0;
    uint32_t rw = 0, rh = 0, inGen = 0;
    bool cut = true;
    float view[16], proj[16], prevView[16], prevProj[16];
    float eye[4], up[4], right[4], fwd[4];
} g_slotCam[dnr::kSlots];
struct Shown { bool valid = false; uint32_t slot = 0; uint64_t frame = 0; } g_shown;   // the picture this frame's pass drew

void ReadDone() {
    uint64_t dv = 0;
    H->channel_read(g_doneCh, &g_done, sizeof g_done, &dv);
    if (dv != g_doneVersion) g_doneVersion = dv, g_doneSeen = Qpc();
}

// Until the half has answered the view sent last (it takes them one at a time): false when it did not in time (a
// half silent for a second: gone, a fault, a reload, its device lost, is not waited for).
bool Answered() {
    ReadDone();
    if (!g_ring.lastSent || g_done.frame >= g_ring.lastSent) return true;
    int64_t w0 = Qpc();
    double budget = g_doneSeen && Ms(g_doneSeen, w0) > 1000 ? 0 : S.waitMs;
    if (!budget) T.silent++;
    while (g_done.frame < g_ring.lastSent) {
        double left = budget - Ms(w0, Qpc());
        if (left <= 0 || !H->channel_wait(g_doneCh, g_doneVersion, uint32_t(std::ceil(left)))) break;
        ReadDone();
    }
    return g_done.frame >= g_ring.lastSent;
}

// The view sent last, answered: its slot's next writer waits for its "done" (when the half signals it).
void Settle() {
    if (!g_ring.lastSent || g_done.frame != g_ring.lastSent || !g_ring.lastPrepared) return;
    g_ring.slotDone[g_ring.lastSlot] = g_done.signalled ? g_done.frame : 0;
    if (g_done.ok) {
        double rt = Ms(g_done.askQpc, g_done.doneQpc);
        T.rtSum += rt, T.rtMax = std::max(T.rtMax, rt), g_windowRtMax = std::max(g_windowRtMax, rt);
        T.workSum += g_done.workMs, T.gpuSum += g_done.gpuMs, T.gpuNrSum += g_done.gpuNrMs, T.gpuSrSum += g_done.gpuSrMs;
    }
}

// DLSS's picture of the view answered last as the frame, into the effects target (alpha 1: the passes after this one
// are laid over it), once this device's GPU has seen the half's "done" of it.
bool Composite(ID3D11DeviceContext *c, const MmlGfxFrame *f, const MmlGfxConstants *k, ID3D11Fence *done) {
    if (!g_ring.lastPrepared || g_done.frame != g_ring.lastSent || !g_done.ok || !g_done.signalled) return false;
    char name[32];
    dnr::TexName(name, sizeof name, g_ring.lastSlot, dnr::kColorOut);
    MmlSharedTexture out{};
    out.size = sizeof out;
    if (!H->gfx_shared_texture(M, name, &out) || out.generation != g_done.outGen || !out.srv || out.width != f->width ||
        out.height != f->height)
        return false;
    G.c4->Wait(done, g_done.frame);
    Constants(c, k, false, S.split ? S.splitAt : 0);
    ID3D11ShaderResourceView *base[2] = {static_cast<ID3D11ShaderResourceView *>(f->depth), static_cast<ID3D11ShaderResourceView *>(f->color)};
    auto *srv = static_cast<ID3D11ShaderResourceView *>(out.srv);
    ID3D11RenderTargetView *target = static_cast<ID3D11RenderTargetView *>(f->target);
    c->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(f->width), float(f->height), 0, 1};
    c->RSSetViewports(1, &vp);
    c->PSSetShaderResources(0, 2, base);
    c->PSSetShaderResources(5, 1, &srv);
    FullScreen(c, f, G.composite);
    ID3D11ShaderResourceView *none = nullptr;
    c->PSSetShaderResources(5, 1, &none);
    T.drawn++;
    g_shown = Shown{true, g_ring.lastSlot, g_done.frame};
    return true;
}

// A view that only asks (the textures at this size, the render size of this mode): nothing is signalled for it.
void Ask(const MmlGfxFrame *f, uint32_t mode, uint32_t up) {
    dnr::View v{};
    v.frame = ++g_frame;
    v.askQpc = Qpc();
    v.outW = f->width, v.outH = f->height;
    v.mode = mode, v.nr = mode == dnr::kModeNr, v.upscale = up;
    H->channel_write(g_viewCh, &v, sizeof v);
    g_ring.lastSent = v.frame, g_ring.lastPrepared = false;
}

void PassFrame(const MmlGfxFrame *f);
void Pass(const MmlGfxFrame *f, void *) {
    if (MML_GFX_HAS(f, in_view) && f->in_view) return;   // (the frame only, not the mods' scene views)
    g_jitUsed = false;
    PassFrame(f);
    if (!g_jitUsed && g_jitHeld) JitterRelease();   // (a frame without DLSS: the game's world drawn still again)
}
void PassFrame(const MmlGfxFrame *f) {
    g_shown.valid = false;
    if (H->mission_state() == MML_MISSION_NONE) return;
    ReadStatus();
    TellStatus(false);
    if (!S.on) {
        g_resetNext = true;
        return;
    }
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *c = static_cast<ID3D11DeviceContext *>(f->context);
    if (G.generation != f->generation || !G.prepare) {
        if (!Make(d, c)) {
            if (!G.c4) H->log(M, MML_LOG_ERROR, "the bridge's device has no Direct3D 11.4 context (ID3D11DeviceContext4): Windows 10 1703 or newer");
            G.Free();
            return;
        }
        G.generation = f->generation;
    }
    const MmlGfxConstants *k = f->k;
    g_bridgeMotion = MML_GFX_HAS(f, motion) && f->motion;
    const bool camera = Cut(k, f);
    const bool cut = g_resetNext || camera;
    const bool havePrev = !cut;
    // the inputs alone, as DLSS would get them at the frame's size (any GPU: nothing goes to the 64-bit half)
    if (S.debug >= 1 && S.debug != 4 && S.debug != 7) {
        Constants(c, k, havePrev, 0);
        ID3D11Resource *color = nullptr;
        static_cast<ID3D11ShaderResourceView *>(f->color)->GetResource(&color);
        if (S.debug >= 5 && color) {   // (the frame before, kept: a copy of the frame's colour each frame)
            D3D11_TEXTURE2D_DESC td{};
            static_cast<ID3D11Texture2D *>(color)->GetDesc(&td);
            D3D11_TEXTURE2D_DESC have{};
            if (G.prev) G.prev->GetDesc(&have);
            if (!G.prev || have.Width != td.Width || have.Height != td.Height || have.Format != td.Format) {
                SafeRelease(G.prevSrv), SafeRelease(G.prev);
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE, td.MiscFlags = 0, td.CPUAccessFlags = 0, td.Usage = D3D11_USAGE_DEFAULT;
                if (SUCCEEDED(d->CreateTexture2D(&td, nullptr, &G.prev))) d->CreateShaderResourceView(G.prev, nullptr, &G.prevSrv);
                if (G.prev) c->CopyResource(G.prev, color);
            }
            c->PSSetShaderResources(6, 1, &G.prevSrv);
        }
        FullScreen(c, f, G.debug);
        if (S.debug >= 5 && color && G.prev) {
            ID3D11ShaderResourceView *none = nullptr;
            c->PSSetShaderResources(6, 1, &none);
            c->CopyResource(G.prev, color);
        }
        if (color) color->Release();
        Keep(k, f);
        g_resetNext = false;
        return;
    }
    if (g_status.stage == dnr::kStarting || g_status.stage == dnr::kNoVulkan || g_status.stage == dnr::kOtherGpu)
        return;   // (no Vulkan device: nothing runs)
    const uint32_t up = UpscaleUsable();
    const uint32_t mode = S.debug == 4 || !S.nr || !NrUsable() ? dnr::kModeCopy : dnr::kModeNr;
    // (nothing of DLSS here: the game's own frame; frame generation alone takes the frame round all the same, for the
    // depth and the motion vectors it needs)
    const bool fgFrames = S.fg && !S.fgStandIn && g_status.fgAvailable;
    if (mode == dnr::kModeCopy && up == dnr::kUpOff && S.debug != 4 && !fgFrames) return;
    T.frames++;
    int64_t t0 = Qpc();

    // the shared fences: a new pair (the 64-bit half started again) begins the ring anew, the frames counted on from
    // where both fences are
    MmlSharedFence ready{}, done{};
    ready.size = done.size = sizeof ready;
    const bool fences = MML_HAS(H, gfx_shared_fence) && H->gfx_shared_fence(M, dnr::kFenceReady, &ready) &&
                        H->gfx_shared_fence(M, dnr::kFenceDone, &done);
    auto *readyF = static_cast<ID3D11Fence *>(ready.fence);
    auto *doneF = static_cast<ID3D11Fence *>(done.fence);
    if (fences && (ready.generation != g_ring.readyGen || done.generation != g_ring.doneGen)) {
        g_ring = Ring{};
        g_ring.readyGen = ready.generation, g_ring.doneGen = done.generation;
        g_frame = std::max({g_frame, readyF->GetCompletedValue(), doneF->GetCompletedValue()});
        g_resetNext = true;
    }
    // the view sent last answered (else this frame is the game's own: late)
    if (!Answered()) {
        T.late++;
        g_resetNext = true;
        return;
    }
    Settle();
    // pipelined: the frame before's picture, now (its DLSS ran while the game drew this frame)
    if (S.pipeline && fences) Composite(c, f, k, doneF);
    // this frame's slot at the size the half wants for this frame's size and mode: else it is asked (the textures are
    // made) and the frame goes without
    const uint32_t s = uint32_t((g_frame + 1) % dnr::kSlots);
    bool have = fences && g_done.renderW && g_done.forW == f->width && g_done.forH == f->height && g_done.forUpscale == up;
    const uint32_t rw = g_done.renderW, rh = g_done.renderH;
    MmlSharedTexture t[dnr::kPerSlot];
    for (uint32_t i = 0; have && i < dnr::kPerSlot; i++) {
        char name[32];
        dnr::TexName(name, sizeof name, s, i);
        t[i] = MmlSharedTexture{};
        t[i].size = sizeof t[i];
        bool in = i != dnr::kColorOut;
        have = H->gfx_shared_texture(M, name, &t[i]) && t[i].width == (in ? rw : f->width) && t[i].height == (in ? rh : f->height);
    }
    if (!have) {
        Ask(f, mode, up);
        T.noTextures++;
        return;
    }
    for (int i = 0; i < 4; i++)
        if (G.inTex[s][i] != t[i].texture || G.inGen[s][i] != t[i].generation) {
            SafeRelease(G.inRtv[s][i]);
            G.inTex[s][i] = static_cast<ID3D11Texture2D *>(t[i].texture);
            G.inGen[s][i] = t[i].generation;
            if (FAILED(d->CreateRenderTargetView(G.inTex[s][i], nullptr, &G.inRtv[s][i]))) { G.FreeIn(); return; }
        }
    // this frame's inputs into its slot, at the render size, once the slot's last work is over (on the GPU)
    dnr::View v{};
    v.frame = g_frame + 1;
    v.outW = f->width, v.outH = f->height;
    v.renderW = rw, v.renderH = rh;
    v.prepared = 1;
    v.slot = s;
    v.inGen = t[dnr::kColorIn].generation, v.outGen = t[dnr::kColorOut].generation;
    v.mode = mode, v.nr = mode == dnr::kModeNr, v.upscale = up;
    v.preset = S.preset, v.style = S.model, v.autoMask = S.autoMask ? 1 : 0, v.srPreset = S.srPreset;
    v.intensity = S.intensity, v.tone = S.tone, v.structure = S.structure, v.skin = S.skin;
    float sample[2], told[2];
    JitterFrame(f, rw, sample, told);
    v.jitter[0] = told[0] * S.jitterSign, v.jitter[1] = told[1] * S.jitterSign;
    if (g_ring.slotDone[s]) G.c4->Wait(doneF, g_ring.slotDone[s]);
    {   // the slot's frame for frame generation (its camera and the one before)
        SlotCam &sc = g_slotCam[s];
        sc.frame = v.frame, sc.rw = rw, sc.rh = rh, sc.inGen = t[dnr::kDepthIn].generation, sc.cut = cut;
        memcpy(sc.view, f->view, sizeof sc.view), memcpy(sc.proj, f->proj, sizeof sc.proj);
        memcpy(sc.prevView, havePrev ? g_prev.viewM : f->view, sizeof sc.prevView);
        memcpy(sc.prevProj, havePrev ? g_prev.projM : f->proj, sizeof sc.prevProj);
        memcpy(sc.eye, k->eye, sizeof sc.eye), memcpy(sc.up, k->up, sizeof sc.up);
        memcpy(sc.right, k->right, sizeof sc.right), memcpy(sc.fwd, k->fwd, sizeof sc.fwd);
    }
    const float render[4] = {float(f->width) / float(rw), float(f->height) / float(rh), sample[0], sample[1]};
    Constants(c, k, havePrev, 0, render);
    ID3D11ShaderResourceView *base[2] = {static_cast<ID3D11ShaderResourceView *>(f->depth), static_cast<ID3D11ShaderResourceView *>(f->color)};
    c->PSSetShaderResources(0, 2, base);
    c->OMSetRenderTargets(4, G.inRtv[s], nullptr);
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{0, 0, float(rw), float(rh), 0, 1};
    c->RSSetViewports(1, &vp);
    FullScreen(c, f, G.prepare);
    ID3D11RenderTargetView *target = static_cast<ID3D11RenderTargetView *>(f->target);
    c->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT full{0, 0, float(f->width), float(f->height), 0, 1};
    c->RSSetViewports(1, &full);
    G.c4->Signal(readyF, v.frame);
    c->Flush();
    v.reset = cut ? 1 : 0;
    if (cut) T.resets++, T.cuts += camera && !g_resetNext ? 1 : 0;
    v.askQpc = Qpc();
    H->channel_write(g_viewCh, &v, sizeof v);
    g_frame = v.frame;
    g_ring.lastSent = v.frame, g_ring.lastPrepared = true, g_ring.lastSlot = s;
    g_ring.slotDone[s] = 0;   // (until the half answers: then its "done")
    g_renderW = rw, g_renderH = rh, g_outW = f->width, g_outH = f->height;
    if (S.debug == 7 && t[dnr::kMvecIn].srv) {   // (the motion vectors DLSS is given, over the frame)
        Constants(c, k, havePrev, 0, render);
        auto *mv = static_cast<ID3D11ShaderResourceView *>(t[dnr::kMvecIn].srv);
        c->PSSetShaderResources(5, 1, &mv);
        FullScreen(c, f, G.mvecIn);
        ID3D11ShaderResourceView *none = nullptr;
        c->PSSetShaderResources(5, 1, &none);
    }
    Keep(k, f);
    g_resetNext = false;
    T.prepSum += Ms(t0, Qpc());
    // not pipelined: this frame's own picture, waited for
    if (!S.pipeline) {
        if (!Answered()) { T.late++; g_resetNext = true; return; }
        Settle();
        if (!Composite(c, f, k, doneF)) T.none++;
    }
}

// ---- frame generation: at the frame's showing (gfx_show), the frames between this one and the one before
struct FgStats { uint64_t shows, made, pictures, late, none, asks, standIn; double ms, rtSum; } F;
uint64_t g_fgFrame;
dnr::FgDone g_fgDone{};
uint64_t g_fgDoneVersion;
uint32_t g_fgReadyGen, g_fgDoneGen;
uint64_t g_fgLastSent, g_fgLastPicture;

// Row-major 4x4 (Direct3D's row vectors): a * b, and the inverse (false: none).
void Mul4(const float *a, const float *b, float *o) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, sizeof r);
}
bool Inv4(const float *m, float *o) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; j++) std::swap(a[c][j], a[p][j]);
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = a[r][c];
                for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) o[i * 4 + j] = float(a[i][j + 4]);
    return true;
}

void ReadFgDone() {
    uint64_t dv = 0;
    H->channel_read(g_fgDoneCh, &g_fgDone, sizeof g_fgDone, &dv);
    g_fgDoneVersion = dv;
}
// Until the half has answered the view sent last (its work on fg_color queued: written again only after it).
bool FgAnswered() {
    ReadFgDone();
    if (!g_fgLastSent || g_fgDone.frame >= g_fgLastSent) return true;
    int64_t w0 = Qpc();
    while (g_fgDone.frame < g_fgLastSent) {
        double left = S.waitMs - Ms(w0, Qpc());
        if (left <= 0 || !H->channel_wait(g_fgDoneCh, g_fgDoneVersion, uint32_t(std::ceil(left)))) break;
        ReadFgDone();
    }
    return g_fgDone.frame >= g_fgLastSent;
}
void FgAsk(uint32_t w, uint32_t h) {
    dnr::FgView v{};
    v.frame = ++g_fgFrame;
    v.askQpc = Qpc();
    v.outW = w, v.outH = h;
    H->channel_write(g_fgViewCh, &v, sizeof v);
    g_fgLastSent = v.frame;
    F.asks++;
}

// The stand-in (no DLSS-FG on this GPU, the setting fg_stand_in): the frame before as shown and this one blended at
// each picture's time: the showing, its pictures and their pacing tested on any GPU.
int StandIn(ID3D11DeviceContext *c, ID3D11Device *d, const MmlGfxFrame *f, const MmlGfxShow *s, uint32_t n) {
    ID3D11Resource *cur = nullptr;
    static_cast<ID3D11ShaderResourceView *>(f->color)->GetResource(&cur);
    if (!cur) return 0;
    D3D11_TEXTURE2D_DESC td{};
    static_cast<ID3D11Texture2D *>(cur)->GetDesc(&td);
    D3D11_TEXTURE2D_DESC have{};
    if (G.prevShow) G.prevShow->GetDesc(&have);
    bool before = G.prevShow && have.Width == td.Width && have.Height == td.Height && have.Format == td.Format;
    if (!before) {
        SafeRelease(G.prevShowSrv), SafeRelease(G.prevShow);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE, td.MiscFlags = 0, td.CPUAccessFlags = 0, td.Usage = D3D11_USAGE_DEFAULT;
        if (SUCCEEDED(d->CreateTexture2D(&td, nullptr, &G.prevShow))) d->CreateShaderResourceView(G.prevShow, nullptr, &G.prevShowSrv);
    }
    if (before && G.prevShowSrv) {
        c->PSSetShaderResources(6, 1, &G.prevShowSrv);
        for (uint32_t i = 0; i < n; i++) {
            const float at[4] = {float(i + 1) / float(n + 1), 0, 0, 0};
            Constants(c, f->k, false, 0, at);
            auto *rtv = static_cast<ID3D11RenderTargetView *>(s->extra[i]);
            c->OMSetRenderTargets(1, &rtv, nullptr);
            FullScreen(c, f, G.blend);
        }
        ID3D11ShaderResourceView *none = nullptr;
        c->PSSetShaderResources(6, 1, &none);
        c->OMSetRenderTargets(0, nullptr, nullptr);
    }
    if (G.prevShow) c->CopyResource(G.prevShow, cur);
    cur->Release();
    F.standIn++;
    return before ? int(n) : 0;
}

int Show(const MmlGfxShow *s, void *) {
    F.shows++;
    if (!S.on || !S.fg || H->mission_state() == MML_MISSION_NONE) return 0;
    const MmlGfxFrame *f = s->frame;
    auto *c = static_cast<ID3D11DeviceContext *>(f->context);
    auto *d = static_cast<ID3D11Device *>(f->device);
    if (G.generation != f->generation || !G.prepare || !G.c4) return 0;   // (the pass makes them)
    uint32_t n = std::min<uint32_t>(S.fg, s->max_extra);
    if (S.fgStandIn) return StandIn(c, d, f, s, n);
    if (!g_status.fgAvailable || !g_shown.valid) return 0;
    n = std::min(n, std::max(1u, g_status.fgMaxFrames));
    const SlotCam &cam = g_slotCam[g_shown.slot];
    if (cam.frame != g_shown.frame) return 0;
    int64_t t0 = Qpc();
    // the half's fences and textures at the back buffer's size (else asked: it makes them)
    MmlSharedFence rf{}, df{};
    rf.size = df.size = sizeof rf;
    bool fences = H->gfx_shared_fence(M, dnr::kFenceFgReady, &rf) && H->gfx_shared_fence(M, dnr::kFenceFgDone, &df);
    auto *readyF = static_cast<ID3D11Fence *>(rf.fence);
    auto *doneF = static_cast<ID3D11Fence *>(df.fence);
    if (fences && (rf.generation != g_fgReadyGen || df.generation != g_fgDoneGen)) {
        g_fgReadyGen = rf.generation, g_fgDoneGen = df.generation;
        g_fgFrame = std::max({g_fgFrame, readyF->GetCompletedValue(), doneF->GetCompletedValue()});
        g_fgLastSent = 0;
    }
    if (!FgAnswered()) { F.late++; return 0; }
    // (the view before's "fgdone": fg_color is not written again before the half has read it)
    if (g_fgDone.frame == g_fgLastSent && g_fgDone.signalled && doneF) G.c4->Wait(doneF, g_fgDone.frame);
    MmlSharedTexture col{}, out[dnr::kFgMax]{};
    col.size = sizeof col;
    bool have = fences && g_fgDone.outW == f->width && g_fgDone.outH == f->height && H->gfx_shared_texture(M, dnr::kFgColor, &col) &&
                col.width == f->width && col.height == f->height;
    for (uint32_t i = 0; have && i < n; i++) {
        char name[32];
        snprintf(name, sizeof name, "%s%u", dnr::kFgOut, i);
        out[i].size = sizeof out[i];
        have = H->gfx_shared_texture(M, name, &out[i]) && out[i].width == f->width && out[i].height == f->height && out[i].srv;
    }
    if (!have) { FgAsk(f->width, f->height); return 0; }
    if (G.fgColor != col.texture || G.fgColorGen != col.generation) {
        SafeRelease(G.fgColorRtv);
        G.fgColor = static_cast<ID3D11Texture2D *>(col.texture), G.fgColorGen = col.generation;
        if (FAILED(d->CreateRenderTargetView(G.fgColor, nullptr, &G.fgColorRtv))) { G.fgColor = nullptr; return 0; }
    }
    // the back buffer into DLSS-FG's input, signalled
    c->OMSetRenderTargets(1, &G.fgColorRtv, nullptr);
    FullScreen(c, f, G.copyScene);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    dnr::FgView v{};
    v.frame = ++g_fgFrame;
    v.picture = g_shown.frame;
    v.outW = f->width, v.outH = f->height;
    v.renderW = cam.rw, v.renderH = cam.rh;
    v.slot = g_shown.slot;
    v.count = n;
    v.prepared = 1;
    v.reset = cam.cut || g_fgLastPicture + 1 != g_shown.frame ? 1 : 0;
    v.colorGen = col.generation;
    for (uint32_t i = 0; i < n; i++) v.outGen[i] = out[i].generation;
    v.inGen = cam.inGen;
    // the camera: this frame's clip space to the frame before's and back (row vectors: clip -> view -> world -> view'
    // -> clip'), the projection and its inverse
    float iv[16], ip[16], ipv[16], ipp[16], m[16];
    bool cams = Inv4(cam.view, iv) && Inv4(cam.proj, ip) && Inv4(cam.prevView, ipv) && Inv4(cam.prevProj, ipp);
    if (!cams) { v.reset = 1; memcpy(ip, cam.proj, sizeof ip); }
    Mul4(ip, iv, m), Mul4(m, cam.prevView, m), Mul4(m, cam.prevProj, v.clipToPrevClip);
    Mul4(ipp, ipv, m), Mul4(m, cam.view, m), Mul4(m, cam.proj, v.prevClipToClip);
    memcpy(v.viewToClip, cam.proj, sizeof v.viewToClip);
    memcpy(v.clipToView, ip, sizeof v.clipToView);
    memcpy(v.camPos, cam.eye, sizeof v.camPos), memcpy(v.camUp, cam.up, sizeof v.camUp);
    memcpy(v.camRight, cam.right, sizeof v.camRight), memcpy(v.camFwd, cam.fwd, sizeof v.camFwd);
    const float *p = cam.proj;   // (_11 _22 _33 _43: 0, 5, 10, 14)
    v.camNear = p[10] != 0 ? -p[14] / p[10] : 0.1f;
    v.camFar = p[10] != 1 ? p[14] / (1 - p[10]) : 10000.f;
    v.camFov = p[5] != 0 ? 2 * std::atan(1 / p[5]) : 1.2f;
    v.camAspect = p[0] != 0 ? p[5] / p[0] : 1.333f;
    G.c4->Signal(readyF, v.frame);
    c->Flush();
    v.askQpc = Qpc();
    H->channel_write(g_fgViewCh, &v, sizeof v);
    g_fgLastSent = v.frame;
    g_fgLastPicture = g_shown.frame;
    // its pictures: the answer (queued), then this device's GPU waits for "fgdone" and draws them into the targets
    if (!FgAnswered()) { F.late++; return 0; }
    if (g_fgDone.signalled) G.c4->Wait(doneF, v.frame);
    if (!g_fgDone.ok || g_fgDone.count != n) { F.none++; return 0; }
    for (uint32_t i = 0; i < n; i++) {
        if (out[i].generation != g_fgDone.outGen[i]) return F.none++, 0;
        auto *rtv = static_cast<ID3D11RenderTargetView *>(s->extra[i]);
        auto *srv = static_cast<ID3D11ShaderResourceView *>(out[i].srv);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        c->PSSetShaderResources(5, 1, &srv);
        FullScreen(c, f, G.copyIn);
    }
    ID3D11ShaderResourceView *none = nullptr;
    c->PSSetShaderResources(5, 1, &none);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    F.made++, F.pictures += n;
    double ms = Ms(t0, Qpc());
    F.ms = F.ms > 0 ? F.ms * 0.95 + ms * 0.05 : ms;
    return int(n);
}

// The showing taken only while frame generation is on: the bridge copies the back buffer for whoever has it, each frame.
bool g_showHeld;
void UpdateShow() {
    bool want = S.fg > 0;
    if (!MML_HAS(H, gfx_show) || want == g_showHeld) return;
    if (!want) {
        H->gfx_show(M, nullptr, nullptr);
        g_showHeld = false;
    } else if (H->gfx_show(M, Show, nullptr)) {
        g_showHeld = true;
    } else {
        H->log(M, MML_LOG_WARN, "the showing is another mod's (gfx_show): no frame generation");
    }
}

// ---- keys, events, commands
void Switch(bool on) {
    H->config_set(M, "on", on ? "true" : "false");   // (kept: the Options page shows it, the next start keeps it)
    S.on = on;
    g_resetNext = true;
    TellStatus(true);
    CheckBridge(true);
}
void KeyToggle(void *) { Switch(!S.on); }

void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_CONFIG_CHANGED: ReadSettings(); break;
    case MML_EVENT_MISSION_LOADED:
        g_resetNext = true;
        g_toldKey = UINT32_MAX;   // (told again in the new mission: the player sees why it is or is not there)
        g_bridgeTold = false;
        break;
    case MML_EVENT_RENDER_END: {
        CheckBridge(false);
        int64_t now = Qpc();
        if (!g_loggedQpc) g_loggedQpc = now;
        if (Ms(g_loggedQpc, now) >= 60000) { LogStats("the last minute"); g_loggedQpc = now; }
        break;
    }
    case MML_EVENT_MISSION_CLOSE: LogStats("the mission"); g_loggedQpc = Qpc(); break;
    default: break;
    }
}

void Reply(MmlReply *r, const json &j) {
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    H->reply_append(r, s.c_str(), s.size());
}

const char *StageName(uint32_t s) {
    static const char *const n[] = {"starting", "no_vulkan", "ngx_failed", "no_dll", "old_driver", "unsupported", "ready", "failed",
                                    "other_gpu"};
    return s < sizeof n / sizeof n[0] ? n[s] : "?";
}

int CmdState(const char *, MmlReply *r, void *) {
    ReadStatus();
    const dnr::Status &s = g_status;
    char arch[16];
    snprintf(arch, sizeof arch, "0x%X", s.minArch);
    double n = T.drawn ? double(T.drawn) : 1;
    auto r3 = [](double x) { return std::round(x * 1000) / 1000; };
    Reply(r, {{"on", S.on}, {"stage", StageName(s.stage)}, {"message", s.message}, {"gpu", s.gpu},
              {"driver", std::to_string(s.driver[0]) + "." + std::to_string(s.driver[1])}, {"dll", s.dll}, {"dll_path", s.dllPath},
              {"available", s.available != 0}, {"min_arch", arch},
              {"min_driver", std::to_string(s.minDriver[0]) + "." + std::to_string(s.minDriver[1])},
              {"sr", {{"available", s.srAvailable != 0}, {"dll", s.srDll}, {"message", s.srMessage}}},
              {"nr_on", S.nr && NrUsable()}, {"upscale", UpscaleName(UpscaleUsable())}, {"pipeline", S.pipeline},
              {"render", {g_renderW, g_renderH}}, {"frame", {g_outW, g_outH}},
              {"mode", S.debug == 4 ? "round_trip" : S.debug ? "debug_view" : "dlss"}, {"frames", T.frames}, {"drawn", T.drawn},
              {"late", T.late}, {"none", T.none}, {"no_textures", T.noTextures}, {"silent", T.silent},
              {"answer_ms", r3(T.rtSum / n)}, {"answer_max_ms", r3(T.rtMax)}, {"work_ms", r3(T.workSum / n)},
              {"gpu_ms", r3(T.gpuSum / n)}, {"gpu_nr_ms", r3(T.gpuNrSum / n)}, {"gpu_sr_ms", r3(T.gpuSrSum / n)},
              {"pass_ms", r3(T.prepSum / std::max<double>(1, double(T.frames)))},
              {"fg", {{"available", s.fgAvailable != 0}, {"max_frames", s.fgMaxFrames}, {"dll", s.fgDll}, {"frame_gen", S.fg},
                      {"stand_in", S.fgStandIn}, {"shows", F.shows}, {"made", F.made}, {"pictures", F.pictures}, {"late", F.late},
                      {"none", F.none}, {"asks", F.asks}, {"stand_in_frames", F.standIn}, {"ms", r3(F.ms)},
                      {"last_error", g_fgDone.error}}},
              {"last_error", g_done.error}, {"bridge_off", g_bridgeOff}});
    return 0;
}

int CmdToggle(const char *args, MmlReply *r, void *) {
    json a = json::parse(args ? args : "{}", nullptr, false);
    bool on = a.is_object() && a.contains("on") && a["on"].is_boolean() ? a["on"].get<bool>() : !S.on;
    Switch(on);
    Reply(r, {{"on", S.on}, {"stage", StageName(g_status.stage)}, {"message", g_status.message}});
    return 0;
}

int CmdReset(const char *, MmlReply *r, void *) {
    T = Stats{};
    g_logged = Stats{};
    F = FgStats{};
    Reply(r, {{"reset", true}});
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, channel_wait) || !MML_HAS(H, hud_console) || !MML_HAS(H, gfx_shared_fence)) return 1;   // (a loader older than this mod)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_viewCh = H->channel_open(mod, "view", sizeof(dnr::View));
    g_doneCh = H->channel_open(mod, "done", sizeof(dnr::Done));
    g_statusCh = H->channel_open(mod, "status", sizeof(dnr::Status));
    g_fgViewCh = H->channel_open(mod, "fgview", sizeof(dnr::FgView));
    g_fgDoneCh = H->channel_open(mod, "fgdone", sizeof(dnr::FgDone));
    if (!g_viewCh || !g_doneCh || !g_statusCh || !g_fgViewCh || !g_fgDoneCh) return 1;
    ReadSettings();
    // the bridge's motion vectors (the moving cars' and people's motion besides the camera's), where the loader has them
    if (MML_HAS(H, gfx_motion) && !H->gfx_motion(mod, 1)) H->log(mod, MML_LOG_WARN, "no motion vectors from the bridge: the camera's alone");
    for (MmlEventType t : {MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_LOADED, MML_EVENT_RENDER_END, MML_EVENT_MISSION_CLOSE}) H->subscribe(mod, t, OnEvent, nullptr);
    // (first among the passes: its picture is the whole frame, so the other mods' effects are laid over it)
    if (H->gfx_pass(mod, 10, Pass, nullptr) <= 0) {
        H->log(mod, MML_LOG_ERROR, "the graphics bridge refused the pass: dlss_nr needs \"gfx\": true on at the game's start");
        return 1;
    }
    H->command(mod, "state", "{} -> on, stage (ready, unsupported, other_gpu, no_dll, old_driver...), message, sr, upscale, render, fg, drawn, late, gpu_ms", CmdState, nullptr);
    H->command(mod, "toggle", "{on} -> DLSS on or off (no on: switched); the hotkey does the same", CmdToggle, nullptr);
    H->command(mod, "reset", "{} -> the measures started again", CmdReset, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) { G.Free(); }
