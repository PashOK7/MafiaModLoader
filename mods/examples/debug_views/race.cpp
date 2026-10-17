// debug_views, race: the race of mission 6 (06-03 "Before The Race", the autodrom) drawn where it is in the world: a debug view
// for mod and mission authors.
//
// What it draws, over the frame (the lines show through walls):
// - Gates: the circuit's gates (C_racing_gate), each a line across the track between its track node and its outer
//   marker, numbered (0 is the finish). The gate the player heads for is yellow, the others grey.
// - Racers: a pole over each car, its label (place, name, the gate it heads for, its lap of the race), and a line
//   from it to that gate (targets). The player is green.
// The F8 hotkey shows and hides it; Options > Mods > Debug views > Race has the rest. `debug_views.race.state` gives the race as
// data, and checks the layout: each gate's midpoint against the average of its two nodes (the layout is right when
// the error is zero).
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "mml/game/racing.h"
#include <d3d9.h>
#include <d3d11.h>
#include <cstring>
#include "shaders/PsRace.h"
#include "views.h"

#define SECTION "race."   // its settings' keys, under its section of the config

namespace {
const MmlHost *H;
MmlMod *M;
bool g_game;
bool g_on = true;

struct P3 { float x, y, z; };
P3 operator+(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
P3 operator-(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P3 operator*(P3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Len(P3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

// ---- the game's memory, read through the host (a stale pointer is a zero, not a fault)
template <class T> T R(uintptr_t a) {
    T v{};
    if (a) H->mem_read(a, &v, sizeof v);
    return v;
}
std::string Str(uintptr_t p, size_t cap = 32) {
    std::string s;
    char b[32];
    for (size_t at = 0; p && at < cap; at += sizeof b) {
        if (!H->mem_read(p + at, b, sizeof b)) break;
        size_t n = strnlen(b, sizeof b);
        s.append(b, n);
        if (n < sizeof b) break;
    }
    return s;
}
P3 V3At(uintptr_t a) { return R<P3>(a); }
std::string Esc(const std::string &s) {   // a JSON string's contents
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

// the game's own function (on the game thread), behind a guard
bool FrameWorldPos(uintptr_t frame, P3 *out) {
    float w[16];
    if (!frame || !H->frame_world(M, reinterpret_cast<void *>(frame), w)) return false;
    *out = {w[12], w[13], w[14]};
    return true;
}

// ---- the race as read (every frame; the circuit's gates again only when the circuit changes: they never move)
struct Gate { uintptr_t ptr = 0; P3 a{}, b{}, mid{}; int number = 0; };
struct Racer {
    uintptr_t ent = 0, car = 0, gate = 0;
    std::string name;
    bool player = false, posOk = false;
    int place = 0, lap = 0;
    float zone = 0;
    P3 pos{};
};
std::vector<Gate> g_gates;
uintptr_t g_circuit, g_gatesBegin, g_gatesEnd;   // the circuit and its gate vector the gates were read from
std::vector<Racer> g_racers;
struct Seen { uintptr_t ent; int place, lap, gate; };   // the trace's last values of each racer (by entrant)
std::vector<Seen> g_seen;
bool g_race;                     // a race runs and its layout checked out (the player is named as it should be)
std::string g_circuitName;
int g_laps;
bool g_trace;                    // the trace: the race's changes written to the log (config "trace")
float g_midError;                // the largest gap between a gate's midpoint and its nodes' average (0 when right)

// The gates are read again whenever the circuit or its gate vector changes: the race builds them after the circuit
// is chosen (C_racing::GameInit), so a circuit read before that has none.
void ReadGates(uintptr_t circuit, uintptr_t begin, uintptr_t end) {
    g_gates.clear();
    g_circuit = circuit;
    g_gatesBegin = begin;
    g_gatesEnd = end;
    if (!begin || end <= begin || end - begin > 4 * 256) return;
    for (uintptr_t at = begin; at < end; at += 4) {
        Gate g;
        g.ptr = R<uintptr_t>(at);
        if (!g.ptr) continue;
        g.a = V3At(R<uintptr_t>(g.ptr + game::kGateNode));
        g.b = V3At(R<uintptr_t>(g.ptr + game::kGateOuter));
        g.mid = V3At(g.ptr + 8);   // the midpoint: three floats at +8 (the gate's line is between a and b)
        g.number = R<int>(g.ptr + game::kGateNumber);
        g_gates.push_back(g);
    }
}

int GateNumber(uintptr_t gate) {
    for (const Gate &g : g_gates) if (g.ptr == gate) return g.number;
    return -1;
}

// ---- the gate test of the player's car, as the game makes it (C_racing::TestGate, InternTestGate, TestAllGates; 1.2).
// The car's AI probe point (C_car::GetAIPos: a point a few metres from its centre, stored at the car's carProbe) is
// tested against the zone of the racer's current gate, then the next gate's zone, then the previous gate's. A gate's
// zone is the strip between the INFINITE line through its predecessor's two nodes and the infinite line through its
// own two nodes: t is 0 on the first line and 1 on the second, and nothing limits the strip at the gates' ends. A
// probe in none of the three is lost: TestAllGates then takes the first gate, from 0, whose zone holds it. The finish
// zone (gate 26's line to gate 0's) is so a strip across the whole map, and a lap counts when the racer steps from gate
// 0's zone into gate 1's, wherever the probe crosses gate 0's infinite line.
int GateIndex(uintptr_t gate) {
    for (size_t k = 0; k < g_gates.size(); k++) if (g_gates[k].ptr == gate) return static_cast<int>(k);
    return -1;
}
int Wrap(int i) {
    const int n = static_cast<int>(g_gates.size());
    return ((i % n) + n) % n;
}
// the side of p from gate g's infinite line, in the ground plane (x, z): 0 on the line
float Side(const Gate &g, P3 p) {
    float dx = g.b.x - g.a.x, dz = g.b.z - g.a.z;
    return dx * (p.z - g.a.z) - dz * (p.x - g.a.x);
}
// gate i's zone holds p: between its predecessor's line and its own; t = 0 on the first line, 1 on the second
bool ZoneHas(int i, P3 p, float *t) {
    float sp = Side(g_gates[Wrap(i - 1)], p), si = Side(g_gates[i], p);
    float den = sp - si;
    if (std::fabs(den) < 1e-9f) return false;
    float tt = sp / den;
    if (t) *t = tt;
    return tt >= 0 && tt <= 1;
}

struct Analysis {
    bool ok = false;             // the probe point was read (a race runs, the layout has it)
    P3 probe{};                  // the car's AI probe point: the one the game tests
    uintptr_t entrant = 0;
    int cur = -1;                // the player's current gate (its index in the circuit's order: its number)
    int result = -100;           // TestGate's answer: 0 the current gate's zone, 1 the next, -1 the previous, -100 lost
    float t = 0;                 // the zone parameter in the zone that holds the probe (ours)
    float tGame = 0;             // the same, as the game wrote it (the entrant's +0x4C)
    int lost = -1;               // when lost: the gate TestAllGates takes, the first whose zone holds the probe
    float fromCar = 0;           // the probe's distance from the car's centre
};
Analysis g_test;
int g_lapSeen = -1;              // the player's lap at the last frame (-1: no race)
struct Mark { P3 at; DWORD ms; int lap; };
std::vector<Mark> g_lapMarks;        // where the player's lap changed (the probe point then): the laps the game counted
struct Trail { P3 at; int result; };
std::vector<Trail> g_trail;      // the player's probe points, one each time it changes (the game reads it about once a second)
const size_t kTrailMax = 400;

// the player's gate test, every frame the race is read (after Refresh's racers)
void Analyse() {
    const game::RacingLayout *L = game::Racing();
    const int n = static_cast<int>(g_gates.size());
    const Racer *player = nullptr;
    for (const Racer &r : g_racers) if (r.player) player = &r;
    g_test = Analysis();
    if (!player) { g_lapSeen = -1; g_lapMarks.clear(); g_trail.clear(); return; }
    if (!L || !L->carProbe || n < 3 || !player->car) return;
    Analysis &a = g_test;
    a.entrant = player->ent;
    a.probe = R<P3>(player->car + L->carProbe);
    if (Len(a.probe) < 1e-3f) return;   // not computed yet: the game has not tested the car in this race
    a.ok = true;
    a.fromCar = Len(a.probe - player->pos);
    a.cur = GateIndex(player->gate);
    a.tGame = player->zone;
    float t = 0;
    if (a.cur >= 0) {
        if (ZoneHas(a.cur, a.probe, &t)) { a.result = 0; a.t = t; }
        else if (ZoneHas(Wrap(a.cur + 1), a.probe, &t)) { a.result = 1; a.t = t; }
        else if (ZoneHas(Wrap(a.cur - 1), a.probe, &t)) { a.result = -1; a.t = t; }
        else for (int i = 0; i < n; i++) if (ZoneHas(i, a.probe, nullptr)) { a.lost = i; break; }
    }
    if (g_lapSeen >= 0 && player->lap != g_lapSeen && g_lapMarks.size() < 64) g_lapMarks.push_back({a.probe, GetTickCount(), player->lap});
    g_lapSeen = player->lap;
    if (g_trail.empty() || Len(a.probe - g_trail.back().at) > 0.01f) {
        if (g_trail.size() >= kTrailMax) g_trail.erase(g_trail.begin());
        g_trail.push_back({a.probe, a.result});
    }
}

// the gate test as data (the state command)
std::string ProbeJson() {
    if (!g_test.ok) return "null";
    const Analysis &a = g_test;
    char b[320];
    snprintf(b, sizeof b, "{\"probe\":[%.2f,%.2f,%.2f],\"from_car_m\":%.2f,\"gate\":%d,\"result\":%d,\"t\":%.3f,\"t_game\":%.3f,"
             "\"lost_takes\":%d}", a.probe.x, a.probe.y, a.probe.z, a.fromCar, a.cur, a.result, a.t, a.tGame, a.lost);
    return b;
}

void Refresh() {
    g_racers.clear();
    g_race = false;
    const game::RacingLayout *L = game::Racing();
    if (!g_game || !L || H->mission_state() == MML_MISSION_NONE) return;
    uintptr_t racing = R<uintptr_t>(L->global);
    if (!racing) { g_seen.clear(); g_trail.clear(); g_lapMarks.clear(); g_lapSeen = -1; return; }
    uintptr_t circuit = R<uintptr_t>(racing + L->circuit);
    uintptr_t begin = circuit ? R<uintptr_t>(circuit + L->gates) : 0;
    uintptr_t end = circuit ? R<uintptr_t>(circuit + L->gates + 4) : 0;
    if (circuit != g_circuit || begin != g_gatesBegin || end != g_gatesEnd) ReadGates(circuit, begin, end);
    // the layout is checked by the player's name: a race of another build (or a moved field) is not drawn
    if (Str(racing + L->player + game::kEntrantName) != "Tommy Angelo") return;
    g_race = true;
    g_circuitName = Str(circuit);
    g_laps = R<int>(racing + L->laps);
    for (int k = 0; k < game::kRacerSlots; k++) {
        uintptr_t ent = R<uintptr_t>(racing + L->racers + 4 * k);
        if (!ent) continue;
        Racer r;
        r.ent = ent;
        r.name = Str(ent + game::kEntrantName);
        r.player = R<uintptr_t>(ent + game::kEntrantPlayer) != 0;
        r.car = R<uintptr_t>(ent + game::kEntrantCar);
        r.gate = R<uintptr_t>(ent + L->entrantGate);
        r.place = R<int>(ent + L->entrantPlace);
        r.lap = R<int>(ent + L->entrantLap);
        r.zone = R<float>(ent + L->entrantZone);
        if (r.car) r.posOk = FrameWorldPos(R<uintptr_t>(r.car + game::kActorFrame), &r.pos);
        g_racers.push_back(r);
    }
    Analyse();
}

// the gates' midpoints against their nodes (the error the state command reports)
float MidpointError() {
    float worst = 0;
    for (const Gate &g : g_gates) {
        P3 avg = (g.a + g.b) * 0.5f;
        worst = (std::max)(worst, Len(g.mid - avg));
    }
    return worst;
}

// ---- drawing (800x600 units), through the last frame's camera
bool Project(const P3 &w, float *x, float *y) {
    float s[3];
    if (!H->camera_project(M, &w.x, s)) return false;
    *x = s[0];
    *y = s[1];
    return true;
}
// The camera of the last frame (its eye and its forward axis: the ray through the screen's centre), read once a frame.
P3 g_eye{}, g_fwd{};
bool g_cam;
void ReadCamera() {
    float o[3], d[3];
    g_cam = H->camera_ray(M, 400, 300, o, d) != 0;
    g_eye = {o[0], o[1], o[2]};
    g_fwd = {d[0], d[1], d[2]};
}
// A line in the world: one line on the screen, its part behind the camera's near plane cut off first.
void Line3(P3 a, P3 b, uint32_t argb) {
    if (!g_cam) return;
    const float nearPlane = 0.1f;
    auto ahead = [](const P3 &p) { P3 v = p - g_eye; return v.x * g_fwd.x + v.y * g_fwd.y + v.z * g_fwd.z - nearPlane; };
    float da = ahead(a), db = ahead(b);
    if (da < 0 && db < 0) return;
    if (da < 0) a = a + (b - a) * (da / (da - db));
    else if (db < 0) b = b + (a - b) * (db / (db - da));
    float ax, ay, bx, by;
    if (Project(a, &ax, &ay) && Project(b, &bx, &by)) H->draw_line(M, ax, ay, bx, by, argb);
}
// A label above a place; labels that would cover one drawn before them this frame go up past it.
struct Box2 { float x0, y0, x1, y1; };
std::vector<Box2> g_placed;
void Label(const P3 &w, const std::vector<std::string> &lines, uint32_t argb) {
    float x, y;
    if (!Project(w, &x, &y)) return;
    const float size = 13;
    float wmax = 0;
    for (const std::string &l : lines) wmax = (std::max)(wmax, H->draw_text_width(MML_FONT_SMALL, size, l.c_str()));
    float h = size * lines.size() + 4;
    for (int moved = 0; moved < 16; moved++) {
        bool hit = false;
        for (const Box2 &b : g_placed) {
            if (x - wmax / 2 - 4 < b.x1 && x + wmax / 2 + 4 > b.x0 && y - h < b.y1 && y > b.y0) { y = b.y0 - 1; hit = true; }
        }
        if (!hit) break;
    }
    g_placed.push_back({x - wmax / 2 - 4, y - h, x + wmax / 2 + 4, y});
    H->draw_rect(M, x - wmax / 2 - 4, y - h, wmax + 8, h, 0x90000000u, 1);
    for (size_t k = 0; k < lines.size(); k++)
        H->draw_text(M, x, y - h + 2 + size * k, size, k ? 0xFFE0E0E0u : argb, MML_FONT_SMALL, MML_DRAW_CENTER, lines[k].c_str());
}

// ---- the trace (config "trace"): the race's changes for the log (`log_tail`). Each racer's lap, gate and place when
// they change; a gate reached by more than one step from the last one (a jump: the racer was lost or moved); the
// player's car moving far in one frame (a teleport, or a jump across the track); and every 5 s where the player is.
DWORD g_traceAt, g_beatAt;       // when the player's car was last read, when the last summary went out
P3 g_tracePos{};
bool g_tracePosOk;

void Trace() {
    if (!g_trace || !g_race || g_racers.empty()) return;
    const game::RacingLayout *L = game::Racing();
    const int n = static_cast<int>(g_gates.size());
    const DWORD now = GetTickCount();
    void *playerCar = game::PlayerCar();
    const float kmh = playerCar ? game::Car(playerCar).SpeedKmh() : 0.0f;
    if (g_seen.empty()) {
        for (const Racer &r : g_racers) {
            if (!r.player) continue;
            H->log(M, MML_LOG_INFO, "trace: the race of %zu racers on %s, %d laps; the player's entrant 0x%08X (its gate at 0x%08X, "
                   "its lap at 0x%08X, its place at 0x%08X): %d gates", g_racers.size(), g_circuitName.c_str(), g_laps,
                   static_cast<unsigned>(r.ent), static_cast<unsigned>(r.ent + L->entrantGate),
                   static_cast<unsigned>(r.ent + L->entrantLap), static_cast<unsigned>(r.ent + L->entrantPlace), n);
        }
    }
    const Racer *player = nullptr;
    for (const Racer &r : g_racers) {
        if (r.player) player = &r;
        const int gn = GateNumber(r.gate);
        Seen *s = nullptr;
        for (Seen &q : g_seen) if (q.ent == r.ent) s = &q;
        if (!s) {
            g_seen.push_back({r.ent, r.place, r.lap, gn});
            H->log(M, MML_LOG_INFO, "trace %u ms: %s starts: place %d, lap %d, gate %d, zone %.3f",
                   static_cast<unsigned>(now), r.name.c_str(), r.place, r.lap, gn, r.zone);
            continue;
        }
        if (r.lap != s->lap)
            H->log(M, MML_LOG_INFO, "trace %u ms: %s lap %d -> %d (gate %d, place %d%s)", static_cast<unsigned>(now),
                   r.name.c_str(), s->lap, r.lap, gn, r.place, r.player ? ", the player" : "");
        if (gn != s->gate && gn >= 0 && s->gate >= 0 && n > 0) {
            const int fwd = (gn - s->gate + n) % n;   // steps forward from the last gate
            if (fwd != 1 && fwd != n - 1)
                H->log(M, MML_LOG_INFO, "trace %u ms: %s JUMPS: gate %d -> %d (%+d steps, place %d, lap %d)", static_cast<unsigned>(now),
                       r.name.c_str(), s->gate, gn, fwd <= n / 2 ? fwd : fwd - n, r.place, r.lap);
            else if (r.player)
                H->log(M, MML_LOG_INFO, "trace %u ms: the player's gate %d -> %d (%s, lap %d, place %d)", static_cast<unsigned>(now),
                       s->gate, gn, fwd == 1 ? "forward" : "backward", r.lap, r.place);
        }
        if (r.player && r.place != s->place)
            H->log(M, MML_LOG_INFO, "trace %u ms: the player's place %d -> %d (lap %d, gate %d)", static_cast<unsigned>(now),
                   s->place, r.place, r.lap, gn);
        s->place = r.place;
        s->lap = r.lap;
        s->gate = gn;
    }
    if (player && player->posOk) {
        if (g_tracePosOk && now > g_traceAt) {
            const float dist = Len(player->pos - g_tracePos);
            const float speed = dist / ((now - g_traceAt) / 1000.0f);
            if (dist > 10.0f && speed > 80.0f)
                H->log(M, MML_LOG_INFO, "trace %u ms: the player's car JUMPS %.1f m in %u ms (%.0f m/s; the game's speed %.0f km/h) "
                       "from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)", static_cast<unsigned>(now), dist,
                       static_cast<unsigned>(now - g_traceAt), speed, kmh, g_tracePos.x, g_tracePos.y, g_tracePos.z,
                       player->pos.x, player->pos.y, player->pos.z);
        }
        g_tracePos = player->pos;
        g_traceAt = now;
        g_tracePosOk = true;
    }
    if (player && now - g_beatAt >= 5000) {
        g_beatAt = now;
        H->log(M, MML_LOG_INFO, "trace %u ms: the player at (%.1f, %.1f, %.1f), %.0f km/h, gate %d, lap %d, place %d, zone %.3f",
               static_cast<unsigned>(now), player->pos.x, player->pos.y, player->pos.z, kmh, GateNumber(player->gate),
               player->lap, player->place, player->zone);
    }
}

// The gate test's labels (settings "probe"): the probe now, with its answer and the game's t, and the laps counted.
// What lies on the world (the gates, the zone, the probe's disc) is the paint pass; the path and the laps are lines of
// the device in the scene (OnRenderWorld): both are hidden by what stands in front of them.
void DrawProbe(float range, float labels) {
    (void)range;
    if (!g_race || g_gates.empty()) return;
    const Racer *player = nullptr;
    for (const Racer &r : g_racers) if (r.player) player = &r;
    if (!player || !player->posOk) return;
    const P3 pp = player->pos;
    if (g_test.ok && H->config_number(M, SECTION "probe", 1) != 0 && Len(g_test.probe - pp) < labels) {
        const Analysis &a = g_test;
        uint32_t col = a.result == 0 ? 0xFF40FF60u : a.result == 1 ? 0xFF60A0FFu : a.result == -1 ? 0xFFFFA040u : 0xFFFF4040u;
        const char *zone = a.result == 0 ? "in the current gate's zone" : a.result == 1 ? "in the NEXT gate's zone"
                         : a.result == -1 ? "in the PREVIOUS gate's zone" : "in no zone: LOST";
        char l1[128], l2[128], l3[128];
        snprintf(l1, sizeof l1, "probe %.1f m from the car: %s", a.fromCar, zone);
        snprintf(l2, sizeof l2, "gate %d, t %.2f (the game's %.2f)", a.cur, a.t, a.tGame);
        if (a.result == -100) snprintf(l3, sizeof l3, "TestAllGates takes gate %d", a.lost);
        else snprintf(l3, sizeof l3, "%s", "TestGate: the racer keeps this gate");
        Label(a.probe + P3{0, 3.2f, 0}, {l1, l2, l3}, col);
    }
    for (const Mark &m : g_lapMarks) {
        if (Len(m.at - pp) >= labels) continue;
        char b[48];
        snprintf(b, sizeof b, "lap %d counted here", m.lap);
        Label(m.at + P3{0, 8.2f, 0}, {b}, 0xFFFFE040u);
    }
}

// The world's own drawing (settings "trail", "laps"): the probe's path, coloured by its answer, and the poles where a
// lap was counted, as lines of the device drawn into the scene at RENDER_WORLD: depth-tested, so what stands in
// front of them hides them and they lie where the world is.
struct WorldVertex { float x, y, z; uint32_t color; };   // D3DFVF_XYZ | D3DFVF_DIFFUSE
size_t g_worldLines;             // the lines the last RENDER_WORLD drew (for the state)
void OnRenderWorld(const MmlEvent *ev, void *) {
    if (!g_on || !g_race || !MML_EVENT_HAS(ev, world) || !ev->world) return;
    const bool trail = H->config_number(M, SECTION "trail", 1) != 0 && g_trail.size() >= 2;
    const bool laps = H->config_number(M, SECTION "laps", 1) != 0 && !g_lapMarks.empty();
    const Racer *player = nullptr;
    for (const Racer &r : g_racers) if (r.player) player = &r;
    if (!player || !player->posOk) return;
    const P3 pp = player->pos;
    const float range = static_cast<float>(H->config_number(M, SECTION "range", 300));
    auto *d = static_cast<IDirect3DDevice9 *>(H->render_device(9));
    if (!d) return;
    std::vector<WorldVertex> v;
    if (trail) {
        for (size_t k = 1; k < g_trail.size(); k++) {
            const Trail &a = g_trail[k - 1], &b = g_trail[k];
            if (Len(b.at - pp) > range) continue;
            uint32_t col = b.result == 0 ? 0xFF40FF60u : b.result == 1 ? 0xFF60A0FFu : b.result == -1 ? 0xFFFFA040u : 0xFFFF4040u;
            v.push_back({a.at.x, a.at.y + 0.25f, a.at.z, col});
            v.push_back({b.at.x, b.at.y + 0.25f, b.at.z, col});
        }
    }
    if (laps) {
        for (const Mark &m : g_lapMarks) {
            if (Len(m.at - pp) > range) continue;
            v.push_back({m.at.x, m.at.y + 0.25f, m.at.z, 0xFFFFE040u});
            v.push_back({m.at.x, m.at.y + 8.0f, m.at.z, 0xFFFFE040u});
        }
    }
    if (H->config_number(M, SECTION "racers", 1) != 0) {   // a pole over each racer, and a line from him to his gate (targets)
        for (const Racer &r : g_racers) {
            if (!r.posOk || Len(r.pos - pp) > range) continue;
            if (r.player) {   // the player's pole only: the AI racers have none (their targets stay)
                v.push_back({r.pos.x, r.pos.y + 0.2f, r.pos.z, 0xFF40FF60u});
                v.push_back({r.pos.x, r.pos.y + 3.0f, r.pos.z, 0xFF40FF60u});
            }
            if (H->config_number(M, SECTION "targets", 1) != 0) {
                const uint32_t tcol = r.player ? 0xC040FF60u : 0x60FFE040u;
                for (const Gate &g : g_gates) {
                    if (g.ptr != r.gate) continue;
                    v.push_back({r.pos.x, r.pos.y + 1.0f, r.pos.z, tcol});
                    v.push_back({g.mid.x, g.mid.y + 1.0f, g.mid.z, tcol});
                }
            }
        }
    }
    if (g_test.ok && H->config_number(M, SECTION "probe", 1) != 0 && Len(g_test.probe - pp) < range) {
        // a ring on the ground under the probe (the car's height: the probe is about 0.9 m above it)
        const float y = pp.y + 0.05f, r = 0.9f;
        const int segs = 24;
        const uint32_t col = g_test.result == 0 ? 0xFF40FF60u : g_test.result == 1 ? 0xFF60A0FFu
                           : g_test.result == -1 ? 0xFFFFA040u : 0xFFFF4040u;
        for (int k = 0; k < segs; k++) {
            const float a0 = 6.2831853f * k / segs, a1 = 6.2831853f * (k + 1) / segs;
            v.push_back({g_test.probe.x + r * std::cos(a0), y, g_test.probe.z + r * std::sin(a0), col});
            v.push_back({g_test.probe.x + r * std::cos(a1), y, g_test.probe.z + r * std::sin(a1), col});
        }
    }
    const UINT count = static_cast<UINT>(v.size() / 2);
    g_worldLines = count;
    if (!count) return;
    D3DMATRIX world = {};
    world._11 = world._22 = world._33 = world._44 = 1;   // the points are in the world already
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
    d->DrawPrimitiveUP(D3DPT_LINELIST, count, v.data(), sizeof(WorldVertex));
}

void DrawRace(float range, float labels) {
    if (!g_race) return;
    void *player = game::Player();
    P3 pp{};
    if (!player || !FrameWorldPos(R<uintptr_t>(reinterpret_cast<uintptr_t>(player) + game::kActorFrame), &pp)) return;
    ReadCamera();
    uintptr_t playerGate = 0;
    for (const Racer &r : g_racers) if (r.player) playerGate = r.gate;
    if (H->config_number(M, SECTION "gates", 1) != 0) {
        for (const Gate &g : g_gates) {
            float d = Len(g.mid - pp);
            if (d > range) continue;
            uint32_t col = g.ptr == playerGate ? 0xFFFFE040u : g.number == 0 ? 0xFFFFFFFFu : 0x90A0A0A0u;
            if (d < labels) {
                char b[32];
                snprintf(b, sizeof b, g.number == 0 ? "gate 0 (finish)" : "gate %d", g.number);
                Label(g.mid + P3{0, 1.5f, 0}, {b}, col);
            }
        }
    }
    if (H->config_number(M, SECTION "racers", 1) == 0) return;
    for (const Racer &r : g_racers) {
        if (!r.posOk) continue;
        float d = Len(r.pos - pp);
        if (d > range) continue;
        uint32_t col = r.player ? 0xFF40FF60u : 0xFFFFFFFFu;
        if (H->config_number(M, SECTION "targets", 1) != 0) {
        }
        if (d < labels) {
            char a[96], b[96];
            snprintf(a, sizeof a, "%d. %s", r.place, r.name.c_str());
            int gn = GateNumber(r.gate);
            snprintf(b, sizeof b, "gate %d, lap %d of %d, zone %.2f", gn, r.lap + 1, g_laps, r.zone);
            Label(r.pos + P3{0, 3.2f, 0}, {a, b}, col);
        }
    }
}

// ---- the paint: the gates, the player's zone and the car's probe painted on the world's surfaces (race.hlsl), in
// the graphics bridge's pass. The lines are the gates' infinite lines: where a surface of the world meets the vertical
// plane through a gate, so the paint lies on what is there and what stands in front hides it.
struct PaintCb {   // cbuffer RacePaint (b1), its layout in race.hlsl
    float seg[32][4], kind[32][4], colour[5][4], zone[4], probe[4], style[4];
};
static_assert(sizeof(PaintCb) == 1152, "RacePaint's layout");
ID3D11PixelShader *g_paintPs;
ID3D11Buffer *g_paintCb;
unsigned g_paintMadeFor;         // the device generation they were made on
bool g_paint = true;
float g_paintWidth = 2.0f, g_zoneAlpha = 0.10f;

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

bool MakePaint(ID3D11Device *d) {
    SafeRelease(g_paintPs);
    SafeRelease(g_paintCb);
    if (FAILED(d->CreatePixelShader(kPsRace, sizeof kPsRace, nullptr, &g_paintPs))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(PaintCb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_paintCb));
}

void PaintPass(const MmlGfxFrame *f, void *) {
    if (!g_paint || !g_race || g_gates.empty()) return;
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_paintMadeFor != f->generation || !g_paintPs) {
        if (!MakePaint(d)) return;
        g_paintMadeFor = f->generation;
    }
    PaintCb cb{};
    const int n = static_cast<int>((std::min)(g_gates.size(), size_t(32)));
    const int cur = g_test.cur;
    for (int i = 0; i < n; i++) {
        const Gate &g = g_gates[i];
        cb.seg[i][0] = g.a.x;
        cb.seg[i][1] = g.a.z;
        cb.seg[i][2] = g.b.x;
        cb.seg[i][3] = g.b.z;
        int kind = 0;
        if (i == cur) kind = 2;
        else if (i == 0) kind = 1;
        else if (cur >= 0 && (i == Wrap(cur + 1) || i == Wrap(cur - 1))) kind = 3;
        cb.kind[i][0] = static_cast<float>(kind);
    }
    // the kinds 0..3 (another gate, the lap line, the current gate, a neighbour), then the probe
    // kind 0 (the other gates) is not painted: its alpha is 0
    const float colours[5][4] = {{0.00f, 0.00f, 0.00f, 0.00f}, {1.00f, 0.80f, 0.20f, 1.00f}, {0.25f, 1.00f, 0.40f, 1.00f},
                                 {0.40f, 0.65f, 1.00f, 1.00f}, {0.30f, 0.90f, 1.00f, 0.55f}};
    memcpy(cb.colour, colours, sizeof colours);
    if (g_test.ok && cur >= 0) {   // the zone: between the previous gate's line and the current one's
        cb.zone[0] = static_cast<float>(Wrap(cur - 1));
        cb.zone[1] = static_cast<float>(cur);
        cb.zone[2] = 1;
    }
    cb.style[0] = g_paintWidth;
    cb.style[1] = g_zoneAlpha;
    cb.style[2] = 0.9f;
    cb.style[3] = static_cast<float>(n);
    ctx->UpdateSubresource(g_paintCb, 0, nullptr, &cb, 0, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_paintCb);
    ctx->PSSetShader(g_paintPs, nullptr, 0);
    ctx->Draw(3, 0);   // the bridge's full-screen triangle (its vertex shader is bound)
}

void OnFrame(const MmlEvent *, void *) {
    // (its labels are 2D, drawn over everything: not over a menu that stops the world, the settings' own; nothing
    // moves meanwhile)
    if (!g_on || !g_game || H->mission_state() == MML_MISSION_NONE || H->mission_state() == MML_MISSION_PAUSED) return;
    Refresh();
    Trace();
    g_placed.clear();
    const float range = static_cast<float>(H->config_number(M, SECTION "range", 300));
    const float labels = static_cast<float>(H->config_number(M, SECTION "label_range", 120));
    DrawRace(range, labels);
    DrawProbe(range, labels);
}

void ReadSettings() {
    g_on = H->config_number(M, SECTION "show", 1) != 0;
    g_trace = H->config_number(M, SECTION "trace", 0) != 0;
    g_paint = H->config_number(M, SECTION "paint", 1) != 0;
    g_paintWidth = static_cast<float>(H->config_number(M, SECTION "paint_width", 2));
    g_zoneAlpha = static_cast<float>(H->config_number(M, SECTION "zone_alpha", 0.10));
    H->hotkey(M, H->config_string(M, SECTION "hotkey", "F8"), [](void *) { g_on = !g_on; H->config_set(M, SECTION "show", g_on ? "true" : "false"); }, nullptr);
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_LOADED || ev->type == MML_EVENT_MISSION_CLOSE) {
        g_gates.clear();
        g_circuit = g_gatesBegin = g_gatesEnd = 0;
    }
}

// debug_views.race.state: the race as data, and the layout's check
int State(const char *, MmlReply *reply, void *) {
    if (!g_game || !game::Racing()) { H->reply_append(reply, "not a known build of the game", static_cast<size_t>(-1)); return 1; }
    Refresh();
    g_midError = MidpointError();
    std::string out = "{\"mission\":\"" + Esc(H->mission_name()) + "\",\"race\":" + (g_race ? "true" : "false") +
                      ",\"circuit\":\"" + Esc(g_circuitName) + "\",\"laps\":" + std::to_string(g_laps) +
                      ",\"gates\":" + std::to_string(g_gates.size()) + ",\"midpoint_error\":";
    char b[320];
    snprintf(b, sizeof b, "%.4f,\"gate_vector\":[\"0x%08X\",\"0x%08X\"],\"circuit_read\":\"0x%08X\"", g_midError,
             static_cast<unsigned>(g_gatesBegin), static_cast<unsigned>(g_gatesEnd), static_cast<unsigned>(g_circuit));
    out += b;
    out += ",\"gate_list\":[";
    for (size_t k = 0; k < g_gates.size(); k++) {
        const Gate &g = g_gates[k];
        snprintf(b, sizeof b, "%s{\"number\":%d,\"a\":[%.2f,%.2f,%.2f],\"b\":[%.2f,%.2f,%.2f]}", k ? "," : "", g.number,
                 g.a.x, g.a.y, g.a.z, g.b.x, g.b.y, g.b.z);
        out += b;
    }
    out += "],\"trail_points\":" + std::to_string(g_trail.size()) + ",\"world_lines\":" + std::to_string(g_worldLines) +
           ",\"probe\":" + ProbeJson() + ",\"racers\":[";
    for (size_t k = 0; k < g_racers.size(); k++) {
        const Racer &r = g_racers[k];
        snprintf(b, sizeof b, "%s{\"entrant\":\"0x%08X\",\"name\":\"%s\",\"place\":%d,\"lap\":%d,\"gate\":%d,\"zone\":%.3f,"
                 "\"player\":%s,\"car\":\"0x%08X\",\"pos\":[%.2f,%.2f,%.2f]}",
                 k ? "," : "", static_cast<unsigned>(r.ent), Esc(r.name).c_str(), r.place, r.lap, GateNumber(r.gate), r.zone,
                 r.player ? "true" : "false", static_cast<unsigned>(r.car), r.pos.x, r.pos.y, r.pos.z);
        out += b;
    }
    out += "]}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}
} // namespace

int RaceLoad(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, camera_project) || !MML_HAS(host, frame_world)) return 1;   // a loader older than this mod
    g_game = game::Detect(host->module_base("LS3DF.dll"));
    if (g_game && !game::Racing()) g_game = false;
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_LOADED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_RENDER_WORLD, OnRenderWorld, nullptr);
    if (MML_HAS(host, gfx_pass) && host->gfx_pass(mod, 100, PaintPass, nullptr) <= 0)
        host->log(mod, MML_LOG_WARN, "race: the graphics bridge refused the paint pass (gfx_state says why)");
    host->command(mod, "race.state", "the race as data: the circuit, its gates (numbers, nodes), the racers (place, lap, gate, zone, car) and the layout check (midpoint_error 0 when right)", State, nullptr);
    host->log(mod, MML_LOG_INFO, "race loaded (game %s)", host->game_build());
    return 0;
}

void RaceUnload() {
    SafeRelease(g_paintPs);   // the paint's Direct3D 11 objects: the device is the bridge's, these are ours
    SafeRelease(g_paintCb);
}
