// side_picture: a mod in two halves (docs/MODDING.md "64-bit halves"). Its 64-bit half (side_picture64.cpp, run by
// the sidecar) draws a picture with Direct3D 12 into shared textures, one a frame; this 32-bit half shows the latest
// whole one in the frame's top right corner, through the graphics bridge, and measures the round trip.
//
// At each frame's start (FRAME, before the sidecar is told of the frame) it writes the channel "ask": its frame's
// number and the time. The 64-bit half draws that frame's picture into the texture the pass is not reading and
// writes the channel "done". The pass (later in the same frame: the mission's tick and the world's drawing come
// between) reads "done", draws its texture (gfx_shared_texture, opened on the bridge's device) and counts how many
// frames late it is: 0, the picture of this very frame.
//   call side_picture.state -> {"frames", "late": {"0", "1", "2+"}, "none", "round_trip_ms": {"mean", "max"},
//                               "ask_to_done_ms", "texture", "generation"}: what the pass saw
//   call side_picture.side  -> the 64-bit half's own count (side_picture64.cpp)
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/mml.h"
#include "shaders/PsPicture.h"

namespace {
const MmlHost *H;
MmlMod *M;

// the channels' records, the same in both halves (side_picture64.cpp)
struct Ask { uint64_t frame; int64_t qpc; };
struct Done { uint64_t frame; int64_t askQpc; uint32_t texture; uint32_t pad; int64_t doneQpc; };
MmlChannel *g_ask, *g_done;
const char *kNames[2] = {"pic0", "pic1"};

uint64_t g_frame;           // this half's frames (FRAME events)
double g_freqMs;
ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
unsigned g_madeFor;
struct Stats {
    uint64_t frames, late0, late1, late2, none;
    double rtSum, rtMax, workSum;
    uint32_t texture, generation;
} S;

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
double Ms(int64_t from, int64_t to) { return (to - from) / g_freqMs; }

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type != MML_EVENT_FRAME) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    Ask a{++g_frame, t.QuadPart};
    H->channel_write(g_ask, &a, sizeof a);
}

bool Make(ID3D11Device *d) {
    SafeRelease(g_ps);
    SafeRelease(g_cb);
    if (FAILED(d->CreatePixelShader(kPsPicture, sizeof kPsPicture, nullptr, &g_ps))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_cb));
}

void Pass(const MmlGfxFrame *f, void *) {
    if (f->in_view) return;
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_ps) {
        if (!Make(static_cast<ID3D11Device *>(f->device))) return;
        g_madeFor = f->generation;
    }
    S.frames++;
    Done d{};
    uint64_t version = 0;
    if (H->channel_read(g_done, &d, sizeof d, &version) != sizeof d || d.texture > 1) { S.none++; return; }
    MmlSharedTexture t{};
    t.size = sizeof t;
    if (!H->gfx_shared_texture(M, kNames[d.texture], &t) || !t.srv) { S.none++; return; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t late = g_frame - d.frame;
    (late == 0 ? S.late0 : late == 1 ? S.late1 : S.late2)++;
    double rt = Ms(d.askQpc, now.QuadPart);   // from the ask to the picture drawn here
    S.rtSum += rt;
    if (rt > S.rtMax) S.rtMax = rt;
    S.workSum += Ms(d.askQpc, d.doneQpc);
    S.texture = d.texture;
    S.generation = t.generation;
    // the picture in the top right corner, a quarter of the frame's width
    float w = f->width * 0.25f, h = w * t.height / t.width, x = f->width - w - 16, y = 16;
    const float rect[4] = {x, y, w, h};
    ctx->UpdateSubresource(g_cb, 0, nullptr, rect, 0, 0);
    D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    auto *srv = static_cast<ID3D11ShaderResourceView *>(t.srv);
    ctx->PSSetShaderResources(3, 1, &srv);
    ctx->PSSetConstantBuffers(1, 1, &g_cb);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->Draw(3, 0);   // the bridge's full-screen triangle, inside the viewport
    ID3D11ShaderResourceView *none = nullptr;
    ctx->PSSetShaderResources(3, 1, &none);
}

int State(const char *, MmlReply *r, void *) {
    uint64_t seen = S.late0 + S.late1 + S.late2;
    char b[1024];
    snprintf(b, sizeof b,
             "{\"frames\": %llu, \"late\": {\"0\": %llu, \"1\": %llu, \"2+\": %llu}, \"none\": %llu, "
             "\"round_trip_ms\": {\"mean\": %.3f, \"max\": %.3f}, \"ask_to_done_ms\": %.3f, \"texture\": %u, \"generation\": %u}",
             static_cast<unsigned long long>(S.frames), static_cast<unsigned long long>(S.late0), static_cast<unsigned long long>(S.late1),
             static_cast<unsigned long long>(S.late2), static_cast<unsigned long long>(S.none), seen ? S.rtSum / seen : 0.0, S.rtMax,
             seen ? S.workSum / seen : 0.0, S.texture, S.generation);
    H->reply_append(r, b, strlen(b));
    return 0;
}
int Reset(const char *, MmlReply *r, void *) {
    S = Stats{};
    H->reply_append(r, "{\"reset\": true}", static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, gfx_shared_texture)) return 1;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_ask = H->channel_open(mod, "ask", sizeof(Ask));
    g_done = H->channel_open(mod, "done", sizeof(Done));
    if (!g_ask || !g_done) return 1;
    H->subscribe(mod, MML_EVENT_FRAME, OnEvent, nullptr);
    if (!H->gfx_pass(mod, 900, Pass, nullptr)) return 1;
    H->command(mod, "state", "{} -> what the pass saw: frames, how late the pictures were, the round trip", State, nullptr);
    H->command(mod, "reset", "{} -> the measures started again", Reset, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    SafeRelease(g_ps);
    SafeRelease(g_cb);
}
