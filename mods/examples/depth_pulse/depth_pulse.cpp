// depth_pulse: an effect over the frame through the graphics bridge (the bridge's example; docs/MODDING.md "Effects
// over the frame").
//
// What it does: a scanner's wave runs out from the player over the world's surfaces every few seconds (a ring of
// light on the ground and on walls, a grid left behind it), fading into the game's fog as the world does. The pass
// is a pixel shader of the mod's (depth_pulse.hlsl, compiled by the build) drawn by the bridge's full-screen
// triangle into the effects target; it reads the frame's linear depth to find the world point of each pixel
// (frame.hlsli's ViewRay). Nothing of the game's is touched: the bridge composites the target over the frame.
//
// The Direct3D 11 objects are the mod's, made on the bridge's device when the pass first runs and made again when the
// device changes (MmlGfxFrame::generation); released at the unload (the device itself is the bridge's).
//
// Commands: depth_pulse.now (a wave from where the player is now), depth_pulse.status. Settings (config.json): the
// colour, the period, the speed, the radius it fades out at, the ring's width, the grid's cell, the hotkey.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "shaders/PsPulse.h"

namespace {
const MmlHost *H;
MmlMod *M;
int g_pass;

struct Settings {
    float color[3] = {0.3f, 0.85f, 1.0f};
    float strength = 1.2f, period = 4.0f, speed = 25.0f, radius = 80.0f, width = 1.2f, cell = 2.0f;
} S;

ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
unsigned g_madeFor;   // the device generation they were made on
struct Wave { float center[3]; double start; bool on; } g_wave;
unsigned g_frames;
double g_lastRadius;

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

void ReadSettings() {
    const char *c = H->config_string(M, "color", "#4CD9FF");
    if (*c == '#') c++;
    unsigned v = static_cast<unsigned>(strtoul(c, nullptr, 16));
    S.color[0] = ((v >> 16) & 255) / 255.0f;
    S.color[1] = ((v >> 8) & 255) / 255.0f;
    S.color[2] = (v & 255) / 255.0f;
    S.strength = static_cast<float>(H->config_number(M, "strength", 1.2));
    S.period = static_cast<float>(H->config_number(M, "period", 4));
    S.speed = static_cast<float>(H->config_number(M, "speed", 25));
    S.radius = static_cast<float>(H->config_number(M, "radius", 80));
    S.width = static_cast<float>(H->config_number(M, "width", 1.2));
    S.cell = static_cast<float>(H->config_number(M, "cell", 2));
}

double Now() { return GetTickCount64() / 1000.0; }

bool PlayerPos(float out[3]) {
    int state = H->mission_state();
    if ((state != MML_MISSION_RUNNING && state != MML_MISSION_PAUSED) || !strcmp(H->mission_name(), "00menu")) return false;
    void *p = game::Player();
    if (!p) return false;
    void *at = game::PlayerCar() ? game::PlayerCar() : p;
    V3 pos = game::ActorWorldPos(at);
    out[0] = pos.x; out[1] = pos.y; out[2] = pos.z;
    return true;
}

void StartWave() {
    if (PlayerPos(g_wave.center)) {
        g_wave.start = Now();
        g_wave.on = true;
    }
}

bool Make(ID3D11Device *d) {
    SafeRelease(g_ps);
    SafeRelease(g_cb);
    if (FAILED(d->CreatePixelShader(kPsPulse, sizeof kPsPulse, nullptr, &g_ps))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_cb));
}

void Pass(const MmlGfxFrame *f, void *) {
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_ps) {
        if (!Make(d)) return;
        g_madeFor = f->generation;
    }
    double t = Now();
    if (!g_wave.on || t - g_wave.start > S.period) StartWave();
    if (!g_wave.on) return;
    float r = static_cast<float>((t - g_wave.start) * S.speed);
    g_lastRadius = r;
    if (r > S.radius) return;   // faded out: wait for the next wave
    const float cb[12] = {g_wave.center[0], g_wave.center[1], g_wave.center[2], r, S.color[0], S.color[1], S.color[2], S.strength,
                          S.width, S.radius, S.cell, 0};
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_cb);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->Draw(3, 0);   // the bridge's full-screen triangle (its vertex shader is bound)
    g_frames++;
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
}
void OnHotkey(void *) { StartWave(); }

int Now_(const char *, MmlReply *r, void *) {
    StartWave();
    H->reply_append(r, g_wave.on ? "{\"started\":true}" : "{\"started\":false,\"why\":\"no player in a running mission\"}", static_cast<size_t>(-1));
    return 0;
}
int Status(const char *, MmlReply *r, void *) {
    char out[256];
    snprintf(out, sizeof out, "{\"pass\":%d,\"frames\":%u,\"radius\":%.1f,\"center\":[%.1f,%.1f,%.1f],\"generation\":%u}", g_pass, g_frames,
             g_lastRadius, g_wave.center[0], g_wave.center[1], g_wave.center[2], g_madeFor);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, gfx_pass)) return 1;   // a loader without the graphics bridge
    if (!game::Detect(H->module_base("LS3DF.dll"))) {
        H->log(M, MML_LOG_ERROR, "not a build of the game whose addresses are known");
        return 2;
    }
    ReadSettings();
    g_pass = H->gfx_pass(M, 100, Pass, nullptr);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->hotkey(M, H->config_string(M, "hotkey", "F11"), OnHotkey, nullptr);
    H->command(M, "now", "a wave from where the player is now", Now_, nullptr);
    H->command(M, "status", "the pass, frames drawn, the wave", Status, nullptr);
    return g_pass ? 0 : 3;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    SafeRelease(g_ps);   // (the pass is taken back by the loader)
    SafeRelease(g_cb);
}
