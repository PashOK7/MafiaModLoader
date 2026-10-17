// actor_route: the route each walking person follows, the point the game walks him to and the walk he makes, drawn in
// the world (both builds; every person the game walks along an ai_path: the missions' people sent somewhere).
//
// The game walks a person along a route in ai_path::Tick (1.2 0x5026C0, 1.0 0x40C970), thiscall(ai_path *this,
// uint, C_human *owner, bool, bool), ret 0x10, called every tick for the route and its owner. Each tick it hands
// HumanMoveTo (fastcall: ECX the owner, EDX &path->target) the point at +0x74, and counts the waypoint reached when
// the owner is within sqrt(+0x90) of the point at +0x80; then it stages the next one. The route object is freed and
// made anew by the person's actions often (a few seconds apart, a new address each time), so the mod copies what it
// draws inside its hook, while the game holds the object, and never reads a route later.
//
// What is drawn, each part a setting of its own:
// - `route`: the waypoints (+0x24 of each record: the point HumanMoveTo is given) as crosses joined by lines (green;
//   the one he walks to in yellow), and each waypoint's gate, the passage A..B the route was built through (grey);
// - `trail`: his walk over the last `trail_s` seconds of the world's time (cyan), and where he stands now (red);
// - `steer`: the point he walks to now (magenta, with a line from him), the point where the game counts the
//   waypoint reached with its radius (orange circle), and the detour points around a blocker (blue) while he
//   follows them (state 2).
// Only the people within `range` metres of the camera are drawn. One the game has not ticked for a few seconds
// (his route ended, a cutscene) is dropped, as is one the game destroys; every one when the mission closes. Time
// here is the world's: the mission's ticks summed, so a pause stops it.
//
// The layout (read in both builds' code: Tick, GetNormalWaypoint 1.2 0x501C60 / 1.0 0x40BF30, isLastNormalWaypoint,
// GetNextSpecWaypoint 1.2 0x501EF0 / 1.0 0x40C1C0): the normal waypoints are a vector of 0x3C-byte records at +0x1C
// (1.2) / +0x04 (1.0), the detour points a vector of 0x18-byte records at +0x2C (1.2) / +0x14 (1.0); the rest is at
// the same offsets in both. A record: gate A +0x00, gate B +0x0C, the waypoint +0x24, a web node's index +0x34, its
// kind +0x38 (0: reached at the gate's midpoint; 1, 2: at the web node).
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool g_on = true;
bool g_showRoute = true, g_showTrail = true, g_showSteer = true;
float g_range = 150.0f;
double g_trailS = 10.0;          // the walk kept, in seconds of the world's time
double g_clock = 0;              // the world's time: the mission's ticks summed (seconds)

// ---- the game's ai_path, per build
struct Layout {
    const char *build;
    uintptr_t tick;          // ai_path::Tick
    uintptr_t normal;        // vector<ai_path_node>: begin, end at +4
    uintptr_t spec;          // vector<ai_path_specnode>: begin, end at +4
};
constexpr Layout kLayouts[] = {
    {"1.2", 0x005026C0, 0x1C, 0x2C},
    {"1.0", 0x0040C970, 0x04, 0x14},
};
const Layout *g_layout = nullptr;
// both builds' Tick start so (sub esp,18h; push ebx/ebp/esi/edi; mov edi,[esp+30h]; mov bl,20h; mov esi,ecx;
// mov ebp,[edi+68h]): the hook is taken only on these bytes
constexpr uint8_t kTickStart[] = {0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x30,
                                  0xB3, 0x20, 0x8B, 0xF1, 0x8B, 0x6F, 0x68};
// shared by both builds
constexpr uintptr_t kCursor = 0x58;       // int: the normal waypoint followed (-1: none)
constexpr uintptr_t kSpecCursor = 0x5C;   // int: the detour point followed (-1: none)
constexpr uintptr_t kTarget = 0x74;       // float[3]: the point HumanMoveTo walks him to
constexpr uintptr_t kArrive = 0x80;       // float[3]: the point the waypoint is reached at
constexpr uintptr_t kState = 0x8C;        // 0 none, 1 a waypoint, 2 a detour point, 3 invalidated, 4 abandoned
constexpr uintptr_t kArriveR2 = 0x90;     // float: the reach's radius, squared
constexpr uintptr_t kHeaderBytes = 0x94;
constexpr size_t kNodeBytes = 0x3C, kSpecBytes = 0x18;
constexpr uintptr_t kNodeA = 0x00, kNodeB = 0x0C, kNodePos = 0x24, kNodeWeb = 0x34, kNodeKind = 0x38;
constexpr uintptr_t kEntityFrame = 0x68;  // C_actor: its model frame
constexpr size_t kMaxNodes = 1024, kMaxSpec = 64;   // more is not a route (memory that is not one)

typedef char(__fastcall *TickFn)(void *self, void *edx, uint32_t a1, void *owner, uint32_t a3, uint32_t a4);
void *g_orig = nullptr;          // the chain's original (the loader keeps it right while hooks come and go)

constexpr size_t kTrailMax = 4000;     // places a walker keeps at most, whatever `trail_s` says
constexpr size_t kMaxWalkers = 32;
constexpr double kExpireS = 4.0;       // one not ticked for this long (of the world's time) is dropped

struct V { float x, y, z; };
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Len(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V At(const uint8_t *p, uintptr_t off) { V v; std::memcpy(&v, p + off, sizeof v); return v; }

struct Node { V a, b, pos; uint32_t web = 0, kind = 0; };

// one person the game walks along a route, as the last tick left him
struct Walker {
    uintptr_t owner = 0, path = 0;
    unsigned long long ticks = 0;
    double seen = 0;             // the last time the game ticked him (g_clock)
    std::vector<Node> nodes;     // the route's waypoints
    std::vector<V> spec;         // the detour points
    int cursor = -1, specCursor = -1;
    uint32_t state = 0;
    V target{}, arrive{};
    float arriveR = 0;
    V now{};
    bool nowOk = false;
    struct Place { V p; double t; };
    std::vector<Place> trail;    // his places, a little apart, each with the time he was last there
    double routeStart = 0;       // when this route began (the walk since is measured against it)
    V routeOrigin{};             // where he stood then: the route as walked starts there, then its first waypoint
    bool routeOriginOk = false;
    V routeFirst{}, routeLast{}; // the route told by its ends: a new one is often made at the old one's address
};
std::vector<Walker> g_walkers;

V g_eye{};
bool g_eyeOk = false;

// the route object's vector [begin, end) of `stride`-byte records: its count (-1: not a vector one can read)
long long VecCount(const uint8_t *hdr, uintptr_t off, size_t stride, size_t max, uint32_t &begin) {
    uint32_t end = 0;
    std::memcpy(&begin, hdr + off, 4);
    std::memcpy(&end, hdr + off + 4, 4);
    if (!begin) return 0;                                    // empty (never allocated, or freed)
    if (end < begin || (end - begin) % stride) return -1;
    const size_t n = (end - begin) / stride;
    return n > max ? -1 : static_cast<long long>(n);
}

Walker *Find(uintptr_t owner) {
    for (Walker &w : g_walkers) if (w.owner == owner) return &w;
    return nullptr;
}

// copied while the game holds the route (at the end of its tick: what the next tick starts from)
void Snapshot(uintptr_t path, uintptr_t owner) {
    uint8_t hdr[kHeaderBytes];
    if (!H->mem_read(path, hdr, sizeof hdr)) return;
    Walker *w = Find(owner);
    if (!w) {
        if (g_walkers.size() >= kMaxWalkers) {   // the one the game has not ticked the longest makes room
            size_t oldest = 0;
            for (size_t k = 1; k < g_walkers.size(); k++) if (g_walkers[k].seen < g_walkers[oldest].seen) oldest = k;
            g_walkers.erase(g_walkers.begin() + static_cast<std::ptrdiff_t>(oldest));
        }
        g_walkers.emplace_back();
        w = &g_walkers.back();
        w->owner = owner;
    }
    w->path = path;
    w->seen = g_clock;
    w->ticks++;
    std::memcpy(&w->cursor, hdr + kCursor, 4);
    std::memcpy(&w->specCursor, hdr + kSpecCursor, 4);
    std::memcpy(&w->state, hdr + kState, 4);
    w->target = At(hdr, kTarget);
    w->arrive = At(hdr, kArrive);
    float r2 = 0;
    std::memcpy(&r2, hdr + kArriveR2, 4);
    w->arriveR = r2 > 0 && r2 < 1e6f ? std::sqrt(r2) : 0;

    uint32_t begin = 0;
    w->nodes.clear();
    const long long n = VecCount(hdr, g_layout->normal, kNodeBytes, kMaxNodes, begin);
    if (n > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(n) * kNodeBytes);
        if (H->mem_read(begin, raw.data(), raw.size())) {
            w->nodes.resize(static_cast<size_t>(n));
            for (size_t i = 0; i < w->nodes.size(); i++) {
                const uint8_t *r = raw.data() + i * kNodeBytes;
                Node &d = w->nodes[i];
                d.a = At(r, kNodeA);
                d.b = At(r, kNodeB);
                d.pos = At(r, kNodePos);
                std::memcpy(&d.web, r + kNodeWeb, 4);
                std::memcpy(&d.kind, r + kNodeKind, 4);
            }
        }
    }
    if (!w->nodes.empty()) {
        const V first = w->nodes.front().pos, last = w->nodes.back().pos;
        if (Len(first - w->routeFirst) > 0.01f || Len(last - w->routeLast) > 0.01f) {   // another route: the walk measured anew
            w->routeFirst = first;
            w->routeLast = last;
            w->routeStart = g_clock;
            w->routeOrigin = w->now;            // (his place at the last tick: this one's is read below)
            w->routeOriginOk = w->nowOk;
        }
    }
    w->spec.clear();
    const long long s = VecCount(hdr, g_layout->spec, kSpecBytes, kMaxSpec, begin);
    if (s > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(s) * kSpecBytes);
        if (H->mem_read(begin, raw.data(), raw.size()))
            for (size_t i = 0; i < static_cast<size_t>(s); i++) w->spec.push_back(At(raw.data() + i * kSpecBytes, 0));
    }

    uint32_t frame = 0;
    float m[16];
    w->nowOk = H->mem_read(owner + kEntityFrame, &frame, 4) && frame &&
               H->frame_world(M, reinterpret_cast<void *>(static_cast<uintptr_t>(frame)), m);
    if (w->nowOk) {
        w->now = {m[12], m[13], m[14]};
        if (w->trail.empty() || Len(w->now - w->trail.back().p) > 0.2f) {
            w->trail.push_back({w->now, g_clock});
            if (w->trail.size() > kTrailMax) w->trail.erase(w->trail.begin());
        } else {
            w->trail.back().t = g_clock;   // still there: a person standing keeps his place in the trail
        }
    }
}

// the world's time goes on: the walk older than `trail_s` is let go, a walker not ticked for long is dropped
void OnMissionTick(const MmlEvent *ev, void *) {
    g_clock += ev->dt;
    for (size_t k = g_walkers.size(); k-- > 0;) {
        Walker &w = g_walkers[k];
        if (g_clock - w.seen > kExpireS) {
            g_walkers.erase(g_walkers.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        size_t old = 0;
        while (old < w.trail.size() && g_clock - w.trail[old].t > g_trailS) old++;
        if (old) w.trail.erase(w.trail.begin(), w.trail.begin() + static_cast<std::ptrdiff_t>(old));
    }
}

// the game's ai_path::Tick: the chain's original with the same arguments, then the route copied
char __fastcall Detour(void *self, void *edx, uint32_t a1, void *owner, uint32_t a3, uint32_t a4) {
    const char r = reinterpret_cast<TickFn>(g_orig)(self, edx, a1, owner, a3, a4);
    Snapshot(reinterpret_cast<uintptr_t>(self), reinterpret_cast<uintptr_t>(owner));
    return r;
}

void OnFrame(const MmlEvent *, void *) {
    float o[3], d[3];
    g_eyeOk = H->camera_ray(M, 400, 300, o, d) != 0;
    if (g_eyeOk) g_eye = {o[0], o[1], o[2]};
}

// ---- drawing (device lines, as the other world views)
struct Vertex { float x, y, z; uint32_t color; };   // D3DFVF_XYZ | D3DFVF_DIFFUSE
constexpr float kLift = 0.08f;                      // over the ground the waypoints lie on
void Seg(std::vector<Vertex> &v, V a, V b, uint32_t c) {
    v.push_back({a.x, a.y + kLift, a.z, c});
    v.push_back({b.x, b.y + kLift, b.z, c});
}
void Cross(std::vector<Vertex> &v, V p, float r, uint32_t c) {
    Seg(v, p - V{r, 0, 0}, p + V{r, 0, 0}, c);
    Seg(v, p - V{0, r, 0}, p + V{0, r, 0}, c);
    Seg(v, p - V{0, 0, r}, p + V{0, 0, r}, c);
}
void Circle(std::vector<Vertex> &v, V p, float r, uint32_t c) {
    constexpr int kSides = 24;
    for (int i = 0; i < kSides; i++) {
        const float a0 = 6.2831853f * i / kSides, a1 = 6.2831853f * (i + 1) / kSides;
        Seg(v, p + V{r * std::cos(a0), 0, r * std::sin(a0)}, p + V{r * std::cos(a1), 0, r * std::sin(a1)}, c);
    }
}

void OnRenderWorld(const MmlEvent *ev, void *) {
    if (!g_on || !MML_EVENT_HAS(ev, world) || !ev->world) return;
    auto *d = static_cast<IDirect3DDevice9 *>(H->render_device(9));
    if (!d) return;
    std::vector<Vertex> v;
    const uint32_t kGreen = 0xFF40FF60u, kGreenDim = 0xA040FF60u, kGate = 0x90C0C0C0u, kYellow = 0xFFFFE040u;
    const uint32_t kCyan = 0xFF40E0FFu, kRed = 0xFFFF4040u, kMagenta = 0xFFFF40FFu, kOrange = 0xFFFF9020u;
    const uint32_t kBlue = 0xFF4080FFu;
    for (const Walker &w : g_walkers) {
        if (!w.nowOk) continue;
        if (g_eyeOk && Len(w.now - g_eye) > g_range) continue;
        if (g_showRoute) {
            for (size_t k = 0; k < w.nodes.size(); k++) {
                const Node &n = w.nodes[k];
                const bool cur = static_cast<int>(k) == w.cursor;
                Cross(v, n.pos, cur ? 0.6f : 0.35f, cur ? kYellow : kGreen);
                Seg(v, n.a, n.b, kGate);
                if (k) Seg(v, w.nodes[k - 1].pos, n.pos, kGreenDim);
            }
        }
        if (g_showTrail) {
            for (size_t k = 1; k < w.trail.size(); k++) Seg(v, w.trail[k - 1].p, w.trail[k].p, kCyan);
            Cross(v, w.now, 0.4f, kRed);
        }
        if (g_showSteer && (w.state == 1 || w.state == 2)) {
            Cross(v, w.target, 0.7f, kMagenta);
            Seg(v, w.now, w.target, kMagenta);
            if (w.arriveR > 0) Circle(v, w.arrive, w.arriveR, kOrange);
            for (size_t k = 0; k < w.spec.size(); k++) {
                Cross(v, w.spec[k], static_cast<int>(k) == w.specCursor ? 0.5f : 0.3f, kBlue);
                if (k) Seg(v, w.spec[k - 1], w.spec[k], kBlue);
            }
        }
    }
    if (v.empty()) return;
    D3DMATRIX world = {};
    world._11 = world._22 = world._33 = world._44 = 1;
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->SetTexture(0, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->DrawPrimitiveUP(D3DPT_LINELIST, static_cast<UINT>(v.size() / 2), v.data(), sizeof(Vertex));
}

void ReadSettings() {
    g_on = H->config_number(M, "show", 1) != 0;
    g_showRoute = H->config_number(M, "route", 1) != 0;
    g_showTrail = H->config_number(M, "trail", 1) != 0;
    g_showSteer = H->config_number(M, "steer", 1) != 0;
    g_range = static_cast<float>(H->config_number(M, "range", 150));
    g_trailS = std::fmax(0.5, H->config_number(M, "trail_s", 10));
    H->hotkey(M, H->config_string(M, "hotkey", "F6"), [](void *) { g_on = !g_on; H->config_set(M, "show", g_on ? "true" : "false"); }, nullptr);
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_CLOSE) g_walkers.clear();   // its people go with it
    if (ev->type == MML_EVENT_ACTOR_DESTROYED)
        for (size_t k = g_walkers.size(); k-- > 0;)
            if (g_walkers[k].owner == reinterpret_cast<uintptr_t>(ev->actor)) g_walkers.erase(g_walkers.begin() + static_cast<std::ptrdiff_t>(k));
}

// how far a walk lies from its route in the ground plane (x, z): every place of the kept trail since the route began
// against the route as walked (from where he stood when it began, then through the waypoints); the mean and the
// largest distance
void WalkOffset(const Walker &w, float &meanM, float &maxM, size_t &n) {
    meanM = maxM = 0;
    n = 0;
    std::vector<V> line;
    if (w.routeOriginOk) line.push_back(w.routeOrigin);
    for (const Node &node : w.nodes) line.push_back(node.pos);
    if (line.size() < 2) return;
    double sum = 0;
    for (const Walker::Place &place : w.trail) {
        if (place.t < w.routeStart) continue;
        const V p = place.p;
        float best = 1e9f;
        for (size_t k = 1; k < line.size(); k++) {
            const V a = line[k - 1], b = line[k];
            const float dx = b.x - a.x, dz = b.z - a.z;
            const float len2 = dx * dx + dz * dz;
            float t = len2 > 1e-6f ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / len2 : 0.0f;
            t = std::fmax(0.0f, std::fmin(1.0f, t));
            const float ex = p.x - (a.x + dx * t), ez = p.z - (a.z + dz * t);
            best = std::fmin(best, std::sqrt(ex * ex + ez * ez));
        }
        sum += best;
        maxM = std::fmax(maxM, best);
        n++;
    }
    if (n) meanM = static_cast<float>(sum / n);
}

void Vec(std::string &out, V v) {
    char b[96];
    snprintf(b, sizeof b, "[%.3f,%.3f,%.3f]", v.x, v.y, v.z);
    out += b;
}

// actor_route.state: every walker the game ticked, as data
int State(const char *, MmlReply *reply, void *) {
    std::string out = "{\"walkers\":[";
    for (size_t k = 0; k < g_walkers.size(); k++) {
        const Walker &w = g_walkers[k];
        float meanM = 0, maxM = 0;
        size_t samples = 0;
        WalkOffset(w, meanM, maxM, samples);
        const double span = w.trail.empty() ? 0 : g_clock - w.trail.front().t;
        char b[512];
        snprintf(b, sizeof b,
                 "%s{\"owner\":\"0x%08llX\",\"path\":\"0x%08llX\",\"ticks\":%llu,\"s_ago\":%.3f,\"state\":%u,\"cursor\":%d,"
                 "\"spec_cursor\":%d,\"arrive_radius\":%.3f,\"trail\":{\"places\":%zu,\"s\":%.2f,\"keep_s\":%.2f},"
                 "\"walk_vs_route\":{\"samples\":%zu,\"mean_m\":%.3f,\"max_m\":%.3f},",
                 k ? "," : "", static_cast<unsigned long long>(w.owner), static_cast<unsigned long long>(w.path), w.ticks,
                 g_clock - w.seen, w.state, w.cursor, w.specCursor, w.arriveR, w.trail.size(), span, g_trailS,
                 samples, meanM, maxM);
        out += b;
        out += "\"now\":";
        Vec(out, w.now);
        out += ",\"target\":";
        Vec(out, w.target);
        out += ",\"arrive\":";
        Vec(out, w.arrive);
        out += ",\"waypoints\":[";
        for (size_t i = 0; i < w.nodes.size(); i++) {
            const Node &n = w.nodes[i];
            out += i ? ",{\"pos\":" : "{\"pos\":";
            Vec(out, n.pos);
            out += ",\"gate\":[";
            Vec(out, n.a);
            out += ",";
            Vec(out, n.b);
            snprintf(b, sizeof b, "],\"kind\":%u,\"web_node\":%u}", n.kind, n.web);
            out += b;
        }
        out += "],\"detour\":[";
        for (size_t i = 0; i < w.spec.size(); i++) {
            if (i) out += ",";
            Vec(out, w.spec[i]);
        }
        out += "]}";
    }
    out += "]}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, hook) || !MML_HAS(host, frame_world) || !MML_HAS(host, camera_ray)) return 1;   // a loader older than this mod
    for (const Layout &l : kLayouts)
        if (std::strcmp(host->game_build(), l.build) == 0) g_layout = &l;
    if (!g_layout) {
        host->log(mod, MML_LOG_ERROR, "actor_route: game %s has no ai_path layout here", host->game_build());
        return 1;
    }
    uint8_t start[sizeof kTickStart];
    if (!host->mem_read(g_layout->tick, start, sizeof start) || std::memcmp(start, kTickStart, sizeof start) != 0) {
        host->log(mod, MML_LOG_ERROR, "actor_route: ai_path::Tick is not at 0x%08X in this Game.exe", static_cast<unsigned>(g_layout->tick));
        return 1;
    }
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnMissionTick, nullptr);
    host->subscribe(mod, MML_EVENT_RENDER_WORLD, OnRenderWorld, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_ACTOR_DESTROYED, OnEvent, nullptr);
    if (!host->hook(mod, g_layout->tick, reinterpret_cast<void *>(&Detour), &g_orig)) {
        host->log(mod, MML_LOG_ERROR, "actor_route: ai_path::Tick could not be hooked");
        return 1;
    }
    host->command(mod, "state", "every person the game walks along a route: its waypoints and gates, the point he walks to, where the waypoint is reached, the detour points, his walk against the route", State, nullptr);
    host->log(mod, MML_LOG_INFO, "actor_route loaded (game %s)", host->game_build());
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
