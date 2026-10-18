// flow_smoke: a mod in two halves (docs/MODDING.md "64-bit halves") doing what a 32-bit game cannot: NVIDIA Flow 2,
// smoke and fire as gas on a sparse grid, simulated and ray-marched on the GPU (Vulkan) in the 64-bit half
// (flow_smoke64.cpp), in the game's world, in the same frame. This 32-bit half keeps the sources, gives the 64-bit half
// the people and cars that move through the gas (they push it), and in its graphics pass hands Flow this frame (its
// colour and depth) and puts Flow's picture of it, the gas lit and self-shadowed over the world and hidden by it, on
// the screen.
//
//   call flow_smoke.emit  {"kind": "smoke", "ahead": 4, "radius": 0.6, "strength": 1}   a source in front of the player
//                         {"kind": "fire", "at": [x, y, z]}, {"kind": "smoke", "on": "car"} (the player's car: it
//                         follows it, at its back), "velocity": [x, y, z], "seconds": 20 (then it stops) -> {"id"}
//   call flow_smoke.stop  {"id": 1}         a source taken away (its gas stays and fades)
//   call flow_smoke.clear                   everything taken away, the gas too
//   call flow_smoke.wind  {"velocity": [2, 0, 0]}
//   call flow_smoke.state -> {"frames", "drawn", "late", "none", "round_trip_ms", "prepare_ms", "blocks", "steps",
//                             "simulate_ms", "render_ms", "gpu_ms", "sources", "movers", "error"...}
//   F10: fire in front of the player (again: one more); Shift+F10: everything cleared
//
// The frame: at FRAME this half writes "frame" (the sources, the movers near them, the time); the 64-bit half steps
// the gas then (its GPU works while the game draws). In the pass (after the world is drawn) this half draws the frame
// into "color_in" and "depth_in", waits for the GPU to have them, writes "view"; the 64-bit half copies them into Flow
// (CUDA), ray-marches, copies the result into "color_out" and answers in "done"; the pass waits for that answer (a few
// milliseconds at most) and draws "color_out" over the frame. A frame whose answer did not come in time shows no gas.
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "flow_smoke.h"
#include "shaders/PsPrepare.h"
#include "shaders/PsComposite.h"

using nlohmann::json;

namespace {
const MmlHost *H;
MmlMod *M;
MmlChannel *g_frameCh, *g_viewCh, *g_doneCh;
bool g_game;   // game::Detect: the player's car (game.h)
double g_freqMs;
uint64_t g_frame;
double g_time;
uint32_t g_waitMs = 20;   // the pass's budget for this frame's picture

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double Ms(int64_t from, int64_t to) { return (to - from) / g_freqMs; }

// ---- the sources (world coordinates here; about the origin in "frame")
struct Source {
    uint32_t id, kind;
    double at[3];
    float radius, velocity[3], strength;
    void *car;              // followed: a car (its back, at `local` in its frame)
    float local[3];
    double until;           // g_time when it stops (0: never)
};
std::vector<Source> g_sources;
// What a reload keeps (state_put: the game's run, not its saves)
struct Kept {
    uint32_t count, nextId, haveOrigin;
    float ground, wind[3];
    double origin[3];
    Source source[fsm::kMaxSources];
};
uint32_t g_nextId = 1, g_clear = 1;
bool g_haveOrigin;
double g_origin[3];
float g_wind[3];
float g_ground;   // about the origin

struct Stats {
    uint64_t frames, asked, drawn, late, none, noTextures, silent;
    double rtSum, rtMax, prepSum, prepMax;
} S;
fsm::Done g_done;
uint64_t g_doneVersion;   // the last "done" seen, and when (QueryPerformanceCounter)
int64_t g_doneSeen;

bool Ground(const double at[3], double *y) {
    const float from[3] = {float(at[0]), float(at[1] + 2), float(at[2])}, to[3] = {float(at[0]), float(at[1] - 20), float(at[2])};
    MmlHit hit{};
    hit.size = sizeof hit;
    if (!H->world_ray(M, from, to, MML_WORLD_STATIC, nullptr, &hit)) return false;
    *y = hit.pos[1];
    return true;
}

// Where the player is and faces (his model's world matrix): false with no mission.
bool Player(double pos[3], float dir[3]) {
    if (H->mission_state() != MML_MISSION_RUNNING) return false;
    void *p = H->human_player(M);
    float w[16];
    if (!p || !H->frame_world(M, H->actor_frame(M, p), w)) return false;
    float fx = w[8], fz = w[10], fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 0.01f) fx = 0, fz = 1, fl = 1;
    pos[0] = w[12], pos[1] = w[13], pos[2] = w[14];
    dir[0] = fx / fl, dir[1] = 0, dir[2] = fz / fl;
    return true;
}

void SetOrigin(const double at[3]) {
    for (int i = 0; i < 3; i++) g_origin[i] = std::floor(at[i]);
    g_haveOrigin = true;
    g_clear++;   // (a new origin: the old gas would be elsewhere)
    double y = at[1];
    Ground(at, &y);
    g_ground = float(y - g_origin[1]);
}

// A car's place, its axes (forward, right, up) and velocity (vehicle_body); false when it is gone.
bool CarBody(void *car, MmlVehicleBody *b) { return car && H->vehicle_body(M, car, b, nullptr); }

const char *KindName(uint32_t k) { return k == fsm::kFire ? "fire" : k == fsm::kSteam ? "steam" : "smoke"; }

std::string Emit(const json &a, uint32_t *id) {
    std::string kind = a.value("kind", std::string("smoke"));
    Source s{};
    s.kind = kind == "fire" ? fsm::kFire : kind == "steam" ? fsm::kSteam : kind == "smoke" ? fsm::kSmoke : 0;
    if (!s.kind) return "kind: smoke, fire or steam";
    s.radius = std::clamp(a.value("radius", s.kind == fsm::kFire ? 0.5f : 0.8f), 0.05f, 4.0f);
    s.strength = std::clamp(a.value("strength", 1.0f), 0.0f, 10.0f);
    float up = s.kind == fsm::kFire ? 0.5f : s.kind == fsm::kSteam ? 3.0f : 0.8f;
    s.velocity[1] = up;
    if (a.contains("velocity") && a["velocity"].is_array() && a["velocity"].size() == 3)
        for (int i = 0; i < 3; i++) s.velocity[i] = a["velocity"][i].get<float>();
    double pos[3];
    float dir[3];
    if (a.value("on", std::string()) == "car") {
        if (!g_game) return "not a known build of the game (the player's car)";
        s.car = game::PlayerCar();
        if (!s.car) s.car = game::LastCar();
        MmlVehicleBody b;
        if (!CarBody(s.car, &b)) return "the player has no car";
        s.local[2] = -2.4f, s.local[1] = 0.4f;   // (its back, low: where an exhaust or a burning boot is)
        for (int i = 0; i < 3; i++) s.at[i] = b.pos[i] + b.axes[0][i] * s.local[2] + b.axes[2][i] * s.local[1];
    } else if (a.contains("at") && a["at"].is_array() && a["at"].size() == 3) {
        for (int i = 0; i < 3; i++) s.at[i] = a["at"][i].get<double>();
    } else {
        if (!Player(pos, dir)) return "no mission running";
        float ahead = a.value("ahead", 4.0f);
        s.at[0] = pos[0] + dir[0] * ahead, s.at[1] = pos[1], s.at[2] = pos[2] + dir[2] * ahead;
        double y;
        if (Ground(s.at, &y)) s.at[1] = y;
        s.at[1] += s.radius * 0.5f;
    }
    double seconds = a.value("seconds", 0.0);
    s.until = seconds > 0 ? g_time + seconds : 0;
    if (!g_haveOrigin || g_sources.empty()) SetOrigin(s.at);
    s.id = g_nextId++;
    if (g_sources.size() >= fsm::kMaxSources) g_sources.erase(g_sources.begin());   // (the oldest makes room)
    g_sources.push_back(s);
    *id = s.id;
    return "";
}

// ---- the frame's state out: the sources where they are now, the people and cars near them
struct Last { float c[3]; uint64_t frame; };
std::map<const void *, Last> g_last;   // a mover's last centre (its velocity), by its primitive
std::vector<MmlShape> g_shapes(256);
fsm::Frame g_rec;

void OnFrame(float dt) {
    g_frame++;
    bool run = H->mission_state() == MML_MISSION_RUNNING;
    if (run) g_time += dt;
    if (!g_haveOrigin) return;
    fsm::Frame &f = g_rec;
    f.frame = g_frame;
    f.time = g_time;
    f.dt = dt;
    f.run = run;
    memcpy(f.origin, g_origin, sizeof g_origin);
    f.clear = g_clear;
    f.ground = g_ground;
    memcpy(f.wind, g_wind, sizeof g_wind);
    // the scene's sun and ambient light (the game's colours, display gamma: linear here), on the gas
    float toSun[3] = {0.4f, 0.8f, 0.45f}, sun[3] = {0.8f, 0.8f, 0.75f}, ambient[3] = {0.35f, 0.35f, 0.4f};
    H->scene_sun(M, toSun, sun, ambient);
    for (int i = 0; i < 3; i++) {
        f.toSun[i] = toSun[i];
        f.light[i] = std::min(std::pow(std::max(sun[i], 0.0f), 2.2f) + std::pow(std::max(ambient[i], 0.0f), 2.2f), 1.0f);
    }
    // the sources: the ones whose time is over go; a followed car's moves them
    g_sources.erase(std::remove_if(g_sources.begin(), g_sources.end(), [](const Source &s) { return s.until && g_time >= s.until; }),
                    g_sources.end());
    f.sources = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (Source &s : g_sources) {
        fsm::Source &o = f.source[f.sources++];
        o = fsm::Source{};
        o.id = s.id, o.kind = s.kind, o.radius = s.radius, o.strength = s.strength;
        memcpy(o.velocity, s.velocity, sizeof o.velocity);
        MmlVehicleBody b;
        if (s.car && CarBody(s.car, &b)) {
            for (int i = 0; i < 3; i++) {
                s.at[i] = b.pos[i] + b.axes[0][i] * s.local[2] + b.axes[2][i] * s.local[1] + b.axes[1][i] * s.local[0];
                o.velocity[i] += b.velocity[i];
            }
        }
        for (int i = 0; i < 3; i++) {
            o.at[i] = float(s.at[i] - g_origin[i]);
            lo[i] = std::min(lo[i], float(s.at[i])), hi[i] = std::max(hi[i], float(s.at[i]));
        }
    }
    // the movers in the sources' neighbourhood (the gas rises and drifts: 30 m around, 25 m up)
    f.movers = 0;
    if (f.sources) {
        for (int i = 0; i < 3; i += 2) lo[i] -= 30, hi[i] += 30;
        lo[1] = float(g_ground + g_origin[1]) - 3, hi[1] += 25;
        int n = H->world_shapes(M, lo, hi, MML_WORLD_MOVING, g_shapes.data(), int(g_shapes.size()));
        n = std::min(n, int(g_shapes.size()));
        for (int i = 0; i < n && f.movers < fsm::kMaxMovers; i++) {
            const MmlShape &sh = g_shapes[i];
            bool person = sh.classes & 0x100, car = sh.classes & 0x200;
            // (a person's narrow sphere about his waist, a car's whole hull: physx_crates found these the ones)
            if (!(person && sh.kind == MML_SHAPE_SPHERE && (sh.classes & 0x1)) && !(car && sh.kind == MML_SHAPE_BOX && (sh.classes & 0x2)))
                continue;
            fsm::Mover &m = f.mover[f.movers++];
            m = fsm::Mover{};
            m.kind = car ? fsm::kBox : fsm::kSphere;
            for (int k = 0; k < 3; k++) m.center[k] = float(sh.center[k] - g_origin[k]);
            memcpy(m.half, sh.half, sizeof m.half);
            memcpy(m.axes, sh.axes, sizeof m.axes);
            if (person) m.half[0] = 0.45f;   // (his body, not the narrow sphere: what pushes the air)
            Last &last = g_last[sh.prim];
            if (last.frame + 1 == g_frame && dt > 0)
                for (int k = 0; k < 3; k++) m.velocity[k] = (sh.center[k] - last.c[k]) / dt;
            memcpy(last.c, sh.center, sizeof last.c);
            last.frame = g_frame;
        }
        if (g_last.size() > 512)
            for (auto it = g_last.begin(); it != g_last.end();) it = it->second.frame + 2 < g_frame ? g_last.erase(it) : std::next(it);
    }
    H->channel_write(g_frameCh, &f, uint32_t(offsetof(fsm::Frame, mover) + f.movers * sizeof(fsm::Mover)));
}

// ---- drawing (the bridge's Direct3D 11 device)
template <class T> void SafeRelease(T *&p) {
    if (p) p->Release(), p = nullptr;
}
struct Gfx {
    unsigned generation = 0;
    ID3D11PixelShader *prepare = nullptr, *composite = nullptr;
    ID3D11Query *done = nullptr;
    // the render target views on the 64-bit half's "color_in" and "depth_in" (made again when it makes them anew)
    ID3D11Texture2D *inTex[2] = {};
    uint32_t inGen[2] = {};
    ID3D11RenderTargetView *inRtv[2] = {};
    void FreeIn() {
        for (int i = 0; i < 2; i++) SafeRelease(inRtv[i]), inTex[i] = nullptr, inGen[i] = 0;
    }
    void Free() { FreeIn(), SafeRelease(prepare), SafeRelease(composite), SafeRelease(done); }
} G;

bool Make(ID3D11Device *d) {
    G.Free();
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    return SUCCEEDED(d->CreatePixelShader(kPsPrepare, sizeof kPsPrepare, nullptr, &G.prepare)) &&
           SUCCEEDED(d->CreatePixelShader(kPsComposite, sizeof kPsComposite, nullptr, &G.composite)) &&
           SUCCEEDED(d->CreateQuery(&qd, &G.done));
}

void FullScreen(ID3D11DeviceContext *c, const MmlGfxFrame *f, ID3D11PixelShader *ps) {
    c->VSSetShader(static_cast<ID3D11VertexShader *>(f->full_screen), nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->Draw(3, 0);
}

// The camera's view about the origin: world = origin + p, so p * (T(origin) * view) = (p + origin) * view.
void ViewAboutOrigin(const float *view, float *out) {
    memcpy(out, view, 16 * sizeof(float));
    for (int j = 0; j < 4; j++)
        out[12 + j] = float(g_origin[0] * view[j] + g_origin[1] * view[4 + j] + g_origin[2] * view[8 + j] + view[12 + j]);
}

void Ask(const MmlGfxFrame *f, const uint32_t inGen[2]) {
    fsm::View v{};
    v.frame = g_frame;
    v.askQpc = Qpc();
    v.width = f->width, v.height = f->height;
    ViewAboutOrigin(f->view, v.view);
    memcpy(v.proj, f->proj, sizeof v.proj);
    v.inGeneration[0] = inGen[0], v.inGeneration[1] = inGen[1];
    H->channel_write(g_viewCh, &v, sizeof v);
    S.asked++;
}

void Pass(const MmlGfxFrame *f, void *) {
    if (f->in_view || !g_haveOrigin || H->mission_state() == MML_MISSION_NONE) return;
    if (g_sources.empty() && g_done.ok && !g_done.blocks) return;   // (no gas anywhere: nothing to draw, nothing asked)
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *c = static_cast<ID3D11DeviceContext *>(f->context);
    if (G.generation != f->generation || !G.prepare) {
        if (!Make(d)) { G.Free(); return; }
        G.generation = f->generation;
    }
    S.frames++;
    // the shared textures at this frame's size: else the 64-bit half is asked (it makes them) and the frame goes without
    const char *names[3] = {"color_in", "depth_in", "color_out"};
    MmlSharedTexture t[3];
    bool have = true;
    for (int i = 0; i < 3; i++) {
        t[i] = MmlSharedTexture{};
        t[i].size = sizeof t[i];
        have = have && H->gfx_shared_texture(M, names[i], &t[i]) && t[i].width == f->width && t[i].height == f->height;
    }
    if (!have) {
        const uint32_t none[2] = {};
        Ask(f, none);
        S.noTextures++;
        return;
    }
    for (int i = 0; i < 2; i++)
        if (G.inTex[i] != t[i].texture || G.inGen[i] != t[i].generation) {
            SafeRelease(G.inRtv[i]);
            G.inTex[i] = static_cast<ID3D11Texture2D *>(t[i].texture);
            G.inGen[i] = t[i].generation;
            if (FAILED(d->CreateRenderTargetView(G.inTex[i], nullptr, &G.inRtv[i]))) { G.FreeIn(); return; }
        }
    // the frame into "color_in" and "depth_in", whole on the GPU before the 64-bit half reads them
    int64_t t0 = Qpc();
    ID3D11ShaderResourceView *base[2] = {static_cast<ID3D11ShaderResourceView *>(f->depth), static_cast<ID3D11ShaderResourceView *>(f->color)};
    ID3D11Buffer *cb = static_cast<ID3D11Buffer *>(f->constants);
    c->PSSetConstantBuffers(0, 1, &cb);
    c->PSSetShaderResources(0, 2, base);
    c->OMSetRenderTargets(2, G.inRtv, nullptr);
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{0, 0, float(f->width), float(f->height), 0, 1};
    c->RSSetViewports(1, &vp);
    FullScreen(c, f, G.prepare);
    ID3D11RenderTargetView *target = static_cast<ID3D11RenderTargetView *>(f->target);
    c->OMSetRenderTargets(1, &target, nullptr);
    c->End(G.done);
    c->Flush();
    while (c->GetData(G.done, nullptr, 0, 0) == S_FALSE && Ms(t0, Qpc()) < 50) YieldProcessor();
    double prep = Ms(t0, Qpc());
    S.prepSum += prep, S.prepMax = std::max(S.prepMax, prep);
    Ask(f, G.inGen);
    // this frame's picture: waited for within the budget. A 64-bit half that has not answered for half a second
    // (gone: a fault, a reload, its device lost) is not waited for: the frame goes on without the gas until it does
    uint64_t v = 0;
    H->channel_read(g_doneCh, &g_done, sizeof g_done, &v);
    int64_t w0 = Qpc();
    if (v != g_doneVersion) g_doneVersion = v, g_doneSeen = w0;
    double budget = Ms(g_doneSeen, w0) > 500 ? 0 : g_waitMs;
    if (!budget) S.silent++;
    while (g_done.frame < g_frame) {
        double left = budget - Ms(w0, Qpc());
        if (left <= 0 || !H->channel_wait(g_doneCh, v, uint32_t(std::ceil(left)))) break;
        H->channel_read(g_doneCh, &g_done, sizeof g_done, &v);
    }
    if (g_done.frame != g_frame) { S.late++; return; }
    if (!g_done.ok || g_done.outGeneration != t[2].generation || !t[2].srv) { S.none++; return; }
    double rt = Ms(g_done.askQpc, Qpc());
    S.rtSum += rt, S.rtMax = std::max(S.rtMax, rt);
    S.drawn++;
    // the frame with the gas over it, into the effects target (the bridge's blending; alpha 1)
    auto *out = static_cast<ID3D11ShaderResourceView *>(t[2].srv);
    c->PSSetShaderResources(3, 1, &out);
    FullScreen(c, f, G.composite);
    ID3D11ShaderResourceView *none = nullptr;
    c->PSSetShaderResources(3, 1, &none);
}

// ---- events, keys and commands
void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_FRAME: OnFrame(ev->dt); break;
    case MML_EVENT_MISSION_CLOSE:   // (the world goes: the sources and the gas with it)
        g_sources.clear();
        g_clear++;
        g_haveOrigin = false;
        g_last.clear();
        break;
    case MML_EVENT_ACTOR_DESTROYED:   // (a followed car gone: its sources with it)
        g_sources.erase(std::remove_if(g_sources.begin(), g_sources.end(), [ev](const Source &s) { return s.car && s.car == ev->actor; }),
                        g_sources.end());
        break;
    default: break;
    }
}

void Reply(MmlReply *r, const json &j) {
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    H->reply_append(r, s.c_str(), s.size());
}
json Args(const char *args) {
    json a = json::parse(args ? args : "{}", nullptr, false);
    return a.is_object() ? a : json::object();
}

int CmdEmit(const char *args, MmlReply *r, void *) {
    uint32_t id = 0;
    std::string err;
    try {
        err = Emit(Args(args), &id);
    } catch (const json::exception &e) {
        err = e.what();
    }
    if (!err.empty()) { H->reply_error(r, err.c_str()); return 1; }
    const Source &s = g_sources.back();
    Reply(r, {{"id", id}, {"kind", KindName(s.kind)}, {"at", {s.at[0], s.at[1], s.at[2]}}, {"radius", s.radius}, {"follows_car", s.car != nullptr}});
    return 0;
}
int CmdStop(const char *args, MmlReply *r, void *) {
    uint32_t id = Args(args).value("id", 0u);
    auto it = std::find_if(g_sources.begin(), g_sources.end(), [id](const Source &s) { return s.id == id; });
    if (it == g_sources.end()) { H->reply_error(r, "no such source"); return 1; }
    g_sources.erase(it);
    Reply(r, {{"stopped", id}});
    return 0;
}
int CmdClear(const char *, MmlReply *r, void *) {
    g_sources.clear();
    g_clear++;
    Reply(r, {{"cleared", true}});
    return 0;
}
int CmdWind(const char *args, MmlReply *r, void *) {
    json a = Args(args);
    if (a.contains("velocity") && a["velocity"].is_array() && a["velocity"].size() == 3)
        for (int i = 0; i < 3; i++) g_wind[i] = a["velocity"][i].get<float>();
    Reply(r, {{"wind", {g_wind[0], g_wind[1], g_wind[2]}}});
    return 0;
}
int CmdWait(const char *args, MmlReply *r, void *) {
    json a = Args(args);
    if (a.contains("ms") && a["ms"].is_number()) g_waitMs = std::min(a["ms"].get<uint32_t>(), 100u);
    Reply(r, {{"wait_ms", g_waitMs}});
    return 0;
}
int CmdState(const char *, MmlReply *r, void *) {
    double k = S.drawn ? 1.0 / S.drawn : 0, kp = S.asked ? 1.0 / S.asked : 0;
    json sources = json::array();
    for (const Source &s : g_sources)
        sources.push_back({{"id", s.id}, {"kind", KindName(s.kind)}, {"at", {s.at[0], s.at[1], s.at[2]}}, {"radius", s.radius},
                           {"follows_car", s.car != nullptr}});
    std::string err(g_done.error, strnlen(g_done.error, sizeof g_done.error));
    Reply(r, {{"frames", S.frames}, {"asked", S.asked}, {"drawn", S.drawn}, {"late", S.late}, {"none", S.none},
              {"no_textures", S.noTextures}, {"half_silent", S.silent}, {"wait_budget_ms", g_waitMs},
              {"round_trip_ms", {{"mean", S.rtSum * k}, {"max", S.rtMax}}},
              {"prepare_ms", {{"mean", S.prepSum * kp}, {"max", S.prepMax}}},
              {"blocks", g_done.blocks}, {"steps", g_done.steps}, {"simulate_ms", g_done.simulateMs}, {"render_ms", g_done.renderMs},
              {"gpu_ms", g_done.gpuMs}, {"sources", sources}, {"movers", g_rec.movers},
              {"origin", g_haveOrigin ? json{g_origin[0], g_origin[1], g_origin[2]} : json()}, {"ground", g_ground},
              {"wind", {g_wind[0], g_wind[1], g_wind[2]}}, {"error", err}});
    return 0;
}
int CmdReset(const char *, MmlReply *r, void *) {
    S = Stats{};
    Reply(r, {{"reset", true}});
    return 0;
}

void KeyFire(void *) {
    uint32_t id;
    std::string err = Emit({{"kind", "fire"}}, &id);
    if (!err.empty()) H->log(M, MML_LOG_WARN, "F10: %s", err.c_str());
}
void KeyClear(void *) {
    g_sources.clear();
    g_clear++;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, channel_wait) || !MML_HAS(H, vehicle_body)) return 1;   // (a loader older than this mod)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_game = game::Detect(host->module_base("LS3DF.dll"));
    g_frameCh = H->channel_open(mod, "frame", sizeof(fsm::Frame));
    g_viewCh = H->channel_open(mod, "view", sizeof(fsm::View));
    g_doneCh = H->channel_open(mod, "done", sizeof(fsm::Done));
    if (!g_frameCh || !g_viewCh || !g_doneCh) return 1;
    Kept k{};
    if (H->state_get(mod, "sources", &k, sizeof k) == sizeof k && k.count <= fsm::kMaxSources) {   // (a reload)
        g_sources.assign(k.source, k.source + k.count);
        g_nextId = k.nextId, g_haveOrigin = k.haveOrigin, g_ground = k.ground;
        memcpy(g_origin, k.origin, sizeof g_origin);
        memcpy(g_wind, k.wind, sizeof g_wind);
    }
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_MISSION_CLOSE, MML_EVENT_ACTOR_DESTROYED}) H->subscribe(mod, t, OnEvent, nullptr);
    // (early among the passes: its picture is the whole frame, so the passes after it are laid over the gas)
    if (H->gfx_pass(mod, 50, Pass, nullptr) <= 0) {
        H->log(mod, MML_LOG_ERROR, "the graphics bridge refused the pass: flow_smoke needs \"gfx\": true on at the game's start");
        return 1;
    }
    H->command(mod, "emit", "{kind: smoke|fire|steam, ahead | at | on: \"car\", radius, strength, velocity, seconds} -> {id}: a source of gas (NVIDIA Flow on the GPU)", CmdEmit, nullptr);
    H->command(mod, "stop", "{id} -> that source taken away (its gas fades)", CmdStop, nullptr);
    H->command(mod, "clear", "{} -> every source and all the gas taken away", CmdClear, nullptr);
    H->command(mod, "wind", "{velocity: [x, y, z]} -> the air's own motion, m/s", CmdWind, nullptr);
    H->command(mod, "wait", "{ms} -> how long the pass waits for this frame's picture (at most 100)", CmdWait, nullptr);
    H->command(mod, "state", "{} -> frames drawn, late, the round trip, Flow's blocks and times, the sources", CmdState, nullptr);
    H->command(mod, "reset", "{} -> the measures started again", CmdReset, nullptr);
    H->hotkey(mod, "F10", KeyFire, nullptr);
    H->hotkey(mod, "Shift+F10", KeyClear, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    // the sources kept for the next load (a reload: the 64-bit half's gas starts again from them)
    Kept k{};
    k.count = uint32_t(std::min(g_sources.size(), size_t(fsm::kMaxSources)));
    k.nextId = g_nextId, k.haveOrigin = g_haveOrigin, k.ground = g_ground;
    memcpy(k.origin, g_origin, sizeof k.origin);
    memcpy(k.wind, g_wind, sizeof k.wind);
    std::copy(g_sources.begin(), g_sources.begin() + k.count, k.source);
    H->state_put(M, "sources", &k, sizeof k);
    G.Free();
}
