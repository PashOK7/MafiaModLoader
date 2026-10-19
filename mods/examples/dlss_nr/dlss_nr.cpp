// dlss_nr: NVIDIA DLSS 5 Neural Rendering ("3D-guided neural rendering") over Mafia's own frame, a mod in two halves
// (docs/MODDING.md "64-bit halves"). DLSS-NR is NVIDIA's model that adds lighting and material detail to the picture
// the engine drew (it does not upscale or make frames); it runs only on 64 bits, through Vulkan, on a GeForce RTX 50
// (Blackwell: NVIDIA's DLL checks the GPU itself). The 64-bit half (dlss_nr64.cpp) runs it; this 32-bit half gives it
// the frame in its graphics pass and puts its picture on the screen, under the HUD.
//
// What DLSS-NR is given: the frame where the world is drawn and the HUD is not yet (in its gamma), its depth, each pixel's
// motion vector (the camera's motion: the depth reprojected through the last frame's camera; the game's moving
// actors are not told apart: a compromise, the game has no motion vectors of its own), and a control mask (how much
// it may change each pixel: the sky's share a setting). The settings are NVIDIA's own (intensity, local tone and
// structure strength, the model, the people's mask), on the game's Options > Mods page, and a key turns it on and off.
//
//   call dlss_nr.state  -> {"on", "stage", "message", "gpu", "driver", "dll", "drawn", "late", "round_trip_ms",
//                           "work_ms", "gpu_ms", ...}: what NVIDIA's NGX said and what the frames cost
//   call dlss_nr.toggle {"on": true}   (no "on": switched)        F12 (the setting "hotkey")
//
// The frame: in the pass this half draws the frame into the four input textures, waits for its GPU to have them,
// writes "view"; the 64-bit half copies them into DLSS-NR's images, evaluates it, copies its picture into "color_out"
// and answers in "done"; the pass waits for that answer (the setting wait_ms at most) and draws "color_out" as the
// frame. The frame waits for DLSS-NR: the game's frame rate is DLSS-NR's.
#include <windows.h>
#include <d3d11.h>
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

using nlohmann::json;

namespace {
const MmlHost *H;
MmlMod *M;
MmlChannel *g_viewCh, *g_doneCh, *g_statusCh;
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
    float intensity = 1, tone = 0.3f, structure = 0.7f, skin = 0.5f, sky = 1;
    uint32_t model = 0, preset = 2;
    bool autoMask = true, split = false, messages = true;
    float splitAt = 0.5f;
    uint32_t debug = 0;      // 0 off, 1 motion vectors, 2 depth, 3 mask, 4 the round trip (the frame through the 64-bit half
                             // untouched), 5 the motion vectors checked (the frame before moved by them, against this one), 6 the
                             // same without them
    uint32_t waitMs = 250;
} S;

float Number(const char *key, double def, float lo, float hi) { return std::clamp(float(H->config_number(M, key, def)), lo, hi); }

void KeyToggle(void *);
void ReadSettings() {
    Settings s;
    s.on = Number("on", 1, 0, 1) >= 0.5f;
    s.hotkey = H->config_string(M, "hotkey", "F12");
    s.intensity = Number("intensity", 1, 0, 1);
    s.tone = Number("tone", 0.3, 0, 1);
    s.structure = Number("structure", 0.7, 0, 1);
    s.model = uint32_t(std::lround(Number("model", 0, 0, 2)));
    s.autoMask = Number("auto_mask", 1, 0, 1) >= 0.5f;
    s.skin = Number("skin", 0.5, 0, 1);
    s.sky = Number("sky", 1, 0, 1);
    s.preset = uint32_t(std::lround(Number("preset", 2, 0, 7)));
    s.split = Number("split", 0, 0, 1) >= 0.5f;
    s.splitAt = Number("split_at", 0.5, 0.05f, 0.95f);
    s.messages = Number("messages", 1, 0, 1) >= 0.5f;
    s.debug = uint32_t(std::lround(Number("debug", 0, 0, 6)));
    s.waitMs = uint32_t(std::lround(Number("wait_ms", 250, 10, 2000)));
    S = s;
    H->hotkey(M, S.hotkey.c_str(), KeyToggle, nullptr);   // (the same function again: the combination replaced)
}

// ---- what the 64-bit half said
dnr::Status g_status{};
uint64_t g_statusVersion;
uint32_t g_toldStage = UINT32_MAX;   // the stage last told on the HUD

void Tell(const char *text, uint32_t argb = 0xFFFFFFFF) {
    if (S.messages && H->mission_state() != MML_MISSION_NONE) H->hud_console(M, text, argb);
}

void ReadStatus() {
    uint64_t v = 0;
    dnr::Status s;
    if (H->channel_read(g_statusCh, &s, sizeof s, &v) == sizeof s && v != g_statusVersion) {
        g_statusVersion = v;
        g_status = s;
        H->log(M, s.stage == dnr::kReady || s.stage == dnr::kStarting ? MML_LOG_INFO : MML_LOG_WARN, "%s", s.message);
    }
}

// What the player is told once NGX has spoken, and again whenever DLSS-NR is switched on.
void TellStatus(bool always) {
    if (g_status.stage == dnr::kStarting) return;
    if (!always && g_toldStage == g_status.stage) return;
    g_toldStage = g_status.stage;
    char b[64];   // (the console's line: 63 characters; the whole sentence is in the log and in dlss_nr.state)
    switch (g_status.stage) {
    case dnr::kReady: Tell(S.on ? "DLSS 5 NR: on" : "DLSS 5 NR: off", 0xFF76B900); return;
    case dnr::kUnsupported: snprintf(b, sizeof b, "DLSS 5 NR: needs a GeForce RTX 50 GPU"); break;
    case dnr::kOtherGpu: snprintf(b, sizeof b, "DLSS 5 NR: the game is not on the NVIDIA GPU (see the log)"); break;
    case dnr::kNoDll: snprintf(b, sizeof b, "DLSS 5 NR: nvngx_dlssnr.dll missing (see the log)"); break;
    case dnr::kOldDriver: snprintf(b, sizeof b, "DLSS 5 NR: update the NVIDIA driver (%u.%u+)", g_status.minDriver[0], g_status.minDriver[1]); break;
    default: snprintf(b, sizeof b, "DLSS 5 NR: not available (see the log)"); break;
    }
    Tell(b, 0xFFFF8040);
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
    if (g_bridgeOff != why) H->log(M, MML_LOG_WARN, "the graphics bridge is off, so DLSS 5 NR gets no frame: %s", why);
    g_bridgeOff = why;
    g_bridgeTold = true;
    Tell("DLSS 5 NR: the graphics bridge is off (see the log)", 0xFFFF8040);
}

// ---- the measures
struct Stats {
    uint64_t frames, drawn, late, none, noTextures, silent;
    double rtSum, rtMax, workSum, gpuSum, prepSum;
} T;

// ---- drawing (the bridge's Direct3D 11 device)
template <class X> void SafeRelease(X *&p) {
    if (p) p->Release(), p = nullptr;
}
struct Consts {   // dlss_nr.hlsl's DlssNr (b1)
    float prevEye[4], prevRight[4], prevUp[4], prevFwd[4], prevProj[4];
    float opts[4];
};
struct Gfx {
    unsigned generation = 0;
    ID3D11PixelShader *prepare = nullptr, *composite = nullptr, *debug = nullptr;
    ID3D11Buffer *cb = nullptr;
    ID3D11Query *done = nullptr;
    ID3D11Texture2D *inTex[4] = {};
    uint32_t inGen[4] = {};
    ID3D11RenderTargetView *inRtv[4] = {};
    ID3D11Texture2D *prev = nullptr;          // the frame before (the debug views' check of the motion vectors)
    ID3D11ShaderResourceView *prevSrv = nullptr;
    void FreeIn() {
        for (int i = 0; i < 4; i++) SafeRelease(inRtv[i]), inTex[i] = nullptr, inGen[i] = 0;
    }
    void Free() {
        FreeIn(), SafeRelease(prepare), SafeRelease(composite), SafeRelease(debug), SafeRelease(cb), SafeRelease(done);
        SafeRelease(prevSrv), SafeRelease(prev);
    }
} G;

bool Make(ID3D11Device *d) {
    G.Free();
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    D3D11_BUFFER_DESC bd{sizeof(Consts), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    return SUCCEEDED(d->CreatePixelShader(kPsPrepare, sizeof kPsPrepare, nullptr, &G.prepare)) &&
           SUCCEEDED(d->CreatePixelShader(kPsComposite, sizeof kPsComposite, nullptr, &G.composite)) &&
           SUCCEEDED(d->CreatePixelShader(kPsDebug, sizeof kPsDebug, nullptr, &G.debug)) &&
           SUCCEEDED(d->CreateBuffer(&bd, nullptr, &G.cb)) && SUCCEEDED(d->CreateQuery(&qd, &G.done));
}

void FullScreen(ID3D11DeviceContext *c, const MmlGfxFrame *f, ID3D11PixelShader *ps) {
    c->VSSetShader(static_cast<ID3D11VertexShader *>(f->full_screen), nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->Draw(3, 0);
}

// The camera of the frame before (the motion vectors), and when DLSS-NR's history is dropped: the first frame, a new
// size, a camera cut (a cutscene's new shot, a teleport: more than 4 m or 30 degrees in one frame), a new model.
struct Camera {
    bool have = false;
    float eye[4], right[4], up[4], fwd[4], proj[4];
    unsigned width = 0, height = 0;
    uint32_t model = UINT32_MAX, preset = UINT32_MAX;
} g_prev;
bool g_resetNext = true;

bool Cut(const MmlGfxConstants *k, const MmlGfxFrame *f) {
    if (!g_prev.have || g_prev.width != f->width || g_prev.height != f->height || g_prev.model != S.model || g_prev.preset != S.preset)
        return true;
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
    g_prev.width = f->width, g_prev.height = f->height;
    g_prev.model = S.model, g_prev.preset = S.preset;
    g_prev.have = true;
}

void Constants(ID3D11DeviceContext *c, const MmlGfxConstants *k, bool havePrev, float split) {
    Consts x{};
    const Camera &p = g_prev;
    memcpy(x.prevEye, havePrev ? p.eye : k->eye, sizeof x.prevEye);
    memcpy(x.prevRight, havePrev ? p.right : k->right, sizeof x.prevRight);
    memcpy(x.prevUp, havePrev ? p.up : k->up, sizeof x.prevUp);
    memcpy(x.prevFwd, havePrev ? p.fwd : k->fwd, sizeof x.prevFwd);
    memcpy(x.prevProj, havePrev ? p.proj : k->proj, sizeof x.prevProj);
    x.opts[0] = S.sky, x.opts[1] = havePrev ? 1.f : 0.f, x.opts[2] = split, x.opts[3] = float(S.debug);
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

dnr::Done g_done{};
uint64_t g_doneVersion;
int64_t g_doneSeen;

void Pass(const MmlGfxFrame *f, void *) {
    if (MML_GFX_HAS(f, in_view) && f->in_view) return;   // (the frame only, not the mods' scene views)
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
        if (!Make(d)) { G.Free(); return; }
        G.generation = f->generation;
    }
    const MmlGfxConstants *k = f->k;
    const bool cut = g_resetNext || Cut(k, f);
    const bool havePrev = !cut;
    // the inputs alone, as DLSS-NR would get them (any GPU: nothing goes to the 64-bit half)
    if (S.debug >= 1 && S.debug != 4) {
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
            c->PSSetShaderResources(4, 1, &G.prevSrv);
        }
        FullScreen(c, f, G.debug);
        if (S.debug >= 5 && color && G.prev) {
            ID3D11ShaderResourceView *none = nullptr;
            c->PSSetShaderResources(4, 1, &none);
            c->CopyResource(G.prev, color);
        }
        if (color) color->Release();
        Keep(k, f);
        g_resetNext = false;
        return;
    }
    const uint32_t mode = S.debug == 4 ? dnr::kModeCopy : dnr::kModeNr;
    if (mode == dnr::kModeNr && g_status.stage != dnr::kReady) return;   // (NGX said no, or has not spoken yet: the game's frame)
    if (g_status.stage == dnr::kStarting || g_status.stage == dnr::kNoVulkan || g_status.stage == dnr::kOtherGpu)
        return;   // (no Vulkan device: no round trip either)
    T.frames++;
    // the shared textures at this frame's size: else the 64-bit half is asked (it makes them) and the frame goes without
    MmlSharedTexture t[dnr::kTextures];
    bool have = true;
    for (uint32_t i = 0; i < dnr::kTextures; i++) {
        t[i] = MmlSharedTexture{};
        t[i].size = sizeof t[i];
        have = have && H->gfx_shared_texture(M, dnr::kTexNames[i], &t[i]) && t[i].width == f->width && t[i].height == f->height;
    }
    dnr::View v{};
    v.frame = ++g_frame;
    v.width = f->width, v.height = f->height;
    v.mode = mode;
    v.preset = S.preset, v.style = S.model, v.autoMask = S.autoMask ? 1 : 0;
    v.intensity = S.intensity, v.tone = S.tone, v.structure = S.structure, v.skin = S.skin;
    if (!have) {
        v.askQpc = Qpc();
        H->channel_write(g_viewCh, &v, sizeof v);
        T.noTextures++;
        return;
    }
    for (int i = 0; i < 4; i++)
        if (G.inTex[i] != t[i].texture || G.inGen[i] != t[i].generation) {
            SafeRelease(G.inRtv[i]);
            G.inTex[i] = static_cast<ID3D11Texture2D *>(t[i].texture);
            G.inGen[i] = t[i].generation;
            if (FAILED(d->CreateRenderTargetView(G.inTex[i], nullptr, &G.inRtv[i]))) { G.FreeIn(); return; }
        }
    // the frame into the inputs, whole on the GPU before the 64-bit half reads them
    int64_t t0 = Qpc();
    Constants(c, k, havePrev, 0);
    ID3D11ShaderResourceView *base[2] = {static_cast<ID3D11ShaderResourceView *>(f->depth), static_cast<ID3D11ShaderResourceView *>(f->color)};
    c->PSSetShaderResources(0, 2, base);
    c->OMSetRenderTargets(4, G.inRtv, nullptr);
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{0, 0, float(f->width), float(f->height), 0, 1};
    c->RSSetViewports(1, &vp);
    FullScreen(c, f, G.prepare);
    ID3D11RenderTargetView *target = static_cast<ID3D11RenderTargetView *>(f->target);
    c->OMSetRenderTargets(1, &target, nullptr);
    c->End(G.done);
    c->Flush();
    while (c->GetData(G.done, nullptr, 0, 0) == S_FALSE && Ms(t0, Qpc()) < 100) YieldProcessor();
    T.prepSum += Ms(t0, Qpc());
    v.reset = cut ? 1 : 0;
    for (int i = 0; i < 4; i++) v.inGeneration[i] = G.inGen[i];
    v.askQpc = Qpc();
    H->channel_write(g_viewCh, &v, sizeof v);
    Keep(k, f);
    g_resetNext = false;
    // this frame's picture, waited for: a 64-bit half silent for a second (gone: a fault, a reload, its device lost)
    // is not waited for, the frame goes on as the game drew it until it answers again
    uint64_t dv = 0;
    H->channel_read(g_doneCh, &g_done, sizeof g_done, &dv);
    int64_t w0 = Qpc();
    if (dv != g_doneVersion) g_doneVersion = dv, g_doneSeen = w0;
    double budget = Ms(g_doneSeen, w0) > 1000 ? 0 : S.waitMs;
    if (!budget) T.silent++;
    while (g_done.frame < g_frame) {
        double left = budget - Ms(w0, Qpc());
        if (left <= 0 || !H->channel_wait(g_doneCh, dv, uint32_t(std::ceil(left)))) break;
        H->channel_read(g_doneCh, &g_done, sizeof g_done, &dv);
        g_doneVersion = dv, g_doneSeen = Qpc();
    }
    if (g_done.frame != g_frame) { T.late++; g_resetNext = true; return; }
    if (!g_done.ok || g_done.outGeneration != t[dnr::kColorOut].generation || !t[dnr::kColorOut].srv) { T.none++; return; }
    double rt = Ms(g_done.askQpc, Qpc());
    T.rtSum += rt, T.rtMax = std::max(T.rtMax, rt);
    T.workSum += g_done.workMs, T.gpuSum += g_done.gpuMs;
    T.drawn++;
    // DLSS-NR's picture as the frame, into the effects target (alpha 1: the passes after this one are laid over it)
    Constants(c, k, false, S.split ? S.splitAt : 0);
    auto *out = static_cast<ID3D11ShaderResourceView *>(t[dnr::kColorOut].srv);
    c->PSSetShaderResources(0, 2, base);
    c->PSSetShaderResources(3, 1, &out);
    FullScreen(c, f, G.composite);
    ID3D11ShaderResourceView *none = nullptr;
    c->PSSetShaderResources(3, 1, &none);
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
        g_toldStage = UINT32_MAX;   // (told again in the new mission: the player sees why it is or is not there)
        g_bridgeTold = false;
        break;
    case MML_EVENT_RENDER_END: CheckBridge(false); break;
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
              {"mode", S.debug == 4 ? "round_trip" : S.debug ? "debug_view" : "nr"}, {"frames", T.frames}, {"drawn", T.drawn},
              {"late", T.late}, {"none", T.none}, {"no_textures", T.noTextures}, {"silent", T.silent},
              {"round_trip_ms", r3(T.rtSum / n)}, {"round_trip_max_ms", r3(T.rtMax)}, {"work_ms", r3(T.workSum / n)},
              {"gpu_ms", r3(T.gpuSum / n)}, {"prepare_ms", r3(T.prepSum / std::max<double>(1, double(T.frames)))},
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
    Reply(r, {{"reset", true}});
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, channel_wait) || !MML_HAS(H, hud_console)) return 1;   // (a loader older than this mod)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_viewCh = H->channel_open(mod, "view", sizeof(dnr::View));
    g_doneCh = H->channel_open(mod, "done", sizeof(dnr::Done));
    g_statusCh = H->channel_open(mod, "status", sizeof(dnr::Status));
    if (!g_viewCh || !g_doneCh || !g_statusCh) return 1;
    ReadSettings();
    for (MmlEventType t : {MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_LOADED, MML_EVENT_RENDER_END}) H->subscribe(mod, t, OnEvent, nullptr);
    // (first among the passes: its picture is the whole frame, so the other mods' effects are laid over it)
    if (H->gfx_pass(mod, 10, Pass, nullptr) <= 0) {
        H->log(mod, MML_LOG_ERROR, "the graphics bridge refused the pass: dlss_nr needs \"gfx\": true on at the game's start");
        return 1;
    }
    H->command(mod, "state", "{} -> on, stage (ready, unsupported, other_gpu, no_dll, old_driver...), message, gpu, driver, dll, drawn, late, round_trip_ms, work_ms, gpu_ms", CmdState, nullptr);
    H->command(mod, "toggle", "{on} -> DLSS 5 NR on or off (no on: switched); the hotkey does the same", CmdToggle, nullptr);
    H->command(mod, "reset", "{} -> the measures started again", CmdReset, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) { G.Free(); }
