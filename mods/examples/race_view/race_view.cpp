// race_view: the race of mission 6 (06-03 "Before The Race", the autodrom) drawn where it is in the world: a debug view
// for mod and mission authors.
//
// What it draws, over the frame (the lines show through walls):
// - Gates: the circuit's gates (C_racing_gate), each a line across the track between its track node and its outer
//   marker, numbered (0 is the finish). The gate the player heads for is yellow, the others grey.
// - Racers: a pole over each car, its label (place, name, the gate it heads for, its lap of the race), and a line
//   from it to that gate (targets). The player is green.
// The F8 hotkey shows and hides it; Options > Mods > Race view has the rest. `race_view.state` gives the race as
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
bool g_race;                     // a race runs and its layout checked out (the player is named as it should be)
std::string g_circuitName;
int g_laps;
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

void Refresh() {
    g_racers.clear();
    g_race = false;
    const game::RacingLayout *L = game::Racing();
    if (!g_game || !L || H->mission_state() == MML_MISSION_NONE) return;
    uintptr_t racing = R<uintptr_t>(L->global);
    if (!racing) return;
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

void DrawRace(float range, float labels) {
    if (!g_race) return;
    void *player = game::Player();
    P3 pp{};
    if (!player || !FrameWorldPos(R<uintptr_t>(reinterpret_cast<uintptr_t>(player) + game::kActorFrame), &pp)) return;
    ReadCamera();
    uintptr_t playerGate = 0;
    for (const Racer &r : g_racers) if (r.player) playerGate = r.gate;
    if (H->config_number(M, "gates", 1) != 0) {
        for (const Gate &g : g_gates) {
            float d = Len(g.mid - pp);
            if (d > range) continue;
            uint32_t col = g.ptr == playerGate ? 0xFFFFE040u : g.number == 0 ? 0xFFFFFFFFu : 0x90A0A0A0u;
            Line3(g.a, g.b, col);
            if (d < labels) {
                char b[32];
                snprintf(b, sizeof b, g.number == 0 ? "gate 0 (finish)" : "gate %d", g.number);
                Label(g.mid + P3{0, 1.5f, 0}, {b}, col);
            }
        }
    }
    if (H->config_number(M, "racers", 1) == 0) return;
    for (const Racer &r : g_racers) {
        if (!r.posOk) continue;
        float d = Len(r.pos - pp);
        if (d > range) continue;
        uint32_t col = r.player ? 0xFF40FF60u : 0xFFFFFFFFu;
        Line3(r.pos + P3{0, 0.2f, 0}, r.pos + P3{0, 3.0f, 0}, col);
        if (H->config_number(M, "targets", 1) != 0) {
            for (const Gate &g : g_gates) if (g.ptr == r.gate) Line3(r.pos + P3{0, 1.0f, 0}, g.mid, r.player ? 0xC040FF60u : 0x60FFE040u);
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

void OnFrame(const MmlEvent *, void *) {
    if (!g_on || !g_game || H->mission_state() == MML_MISSION_NONE) return;
    Refresh();
    g_placed.clear();
    const float range = static_cast<float>(H->config_number(M, "range", 300));
    const float labels = static_cast<float>(H->config_number(M, "label_range", 120));
    DrawRace(range, labels);
}

void ReadSettings() {
    g_on = H->config_number(M, "show", 1) != 0;
    H->hotkey(M, H->config_string(M, "hotkey", "F8"), [](void *) { g_on = !g_on; H->config_set(M, "show", g_on ? "true" : "false"); }, nullptr);
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_LOADED || ev->type == MML_EVENT_MISSION_CLOSE) {
        g_gates.clear();
        g_circuit = g_gatesBegin = g_gatesEnd = 0;
    }
}

// race_view.state: the race as data, and the layout's check
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
    out += "],\"racers\":[";
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

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
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
    host->command(mod, "state", "the race as data: the circuit, its gates (numbers, nodes), the racers (place, lap, gate, zone, car) and the layout check (midpoint_error 0 when right)", State, nullptr);
    host->log(mod, MML_LOG_INFO, "race_view loaded (game %s)", host->game_build());
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
