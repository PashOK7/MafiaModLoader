// debug_views, triggers: the mission scripts' triggers drawn where they are in the world: a debug view for mission and mod
// authors.
//
// What the scripts test, in the colours of its state, over the frame (the lines show through walls):
// - Detectors (C_detector, actor kind 5: an invisible actor running a script; mml/game/game.h kDet*): a label with its
//   name, the command its script is in now (game::ScriptCommandName) and its signal; a sphere for each radius a
//   DETECTOR_INRANGE of its program counts people in (green while the player is counted there); its use point
//   while DETECTOR_WAITFORUSE waits (where the Action key finds it: an upright cylinder, the record's reach and 1.5 m
//   up and down; the Action menu's line); its boxes while DETECTOR_WAITFORHIT waits or DETECTOR_CREATEDYNCOLL made
//   them.
// - Distances (GETACTORSDIST, then IFFLTINRANGE or IF on its answer: a script waiting for someone to come near or go
//   away, in a detector's or a person's script): the line between the two actors, the threshold as a sphere around
//   the one that is not the player, green while the comparison holds (after pet1kk's developer tools, PR 2).
// - Areas (I3D_area, LS3D's frames of type 14: closed meshes the scripts test with ISPOINTINAREA): their edges, green
//   with the player inside (the area's own CheckPoint), brighter when a program's ISPOINTINAREA names it.
//
// The hotkey (F7) shows and hides it; Options > Mods > Debug views > Triggers has the rest. `debug_views.triggers.list` gives what is
// drawn as data. The scripts' programs are read where every detector and person with a script keeps theirs (game.h
// kProgram*), the commands named by the build running (mml/game/script_commands.h). What is near is drawn (`range`),
// a straight edge as one line cut at the camera's near plane: the frame holds 4096 items for every mod together.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "mml/game/script_commands.h"
#include "lines.h"
#include "views.h"

#define SECTION "triggers."   // its settings' keys, under its section of the config

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
std::string Str(uintptr_t p, size_t cap = 96) {
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

// the game's own functions (on the game thread), behind a guard
bool FrameWorldPos(uintptr_t frame, P3 *out) {
    float w[16];
    if (!frame || !H->frame_world(M, reinterpret_cast<void *>(frame), w)) return false;
    *out = {w[12], w[13], w[14]};
    return true;
}
int AreaCheck(uintptr_t area, const P3 &p) {   // 1 inside, 0 not, -1 it faulted
    using Fn = bool(__stdcall *)(void *, const float *);
    __try {
        void **vt = *reinterpret_cast<void ***>(area);
        return reinterpret_cast<Fn>(vt[game::kVtAreaCheckPoint / 4])(reinterpret_cast<void *>(area), &p.x) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool SafeEnumDeep(void *root, std::vector<void *> *out) {
    __try { game::EnumFramesDeep(root, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeListActors(std::vector<void *> *out) {
    __try { game::ListActors(out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const char *GameText(uint32_t id) {
    const game::ExeBuild &e = game::Exe();
    if (!e.textGetOrNull || !e.textDb) return nullptr;
    using Fn = const char *(__thiscall *)(void *, uint32_t);
    __try { return reinterpret_cast<Fn>(e.textGetOrNull)(reinterpret_cast<void *>(e.textDb), id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- what is drawn
struct Detector {
    uintptr_t actor = 0, frame = 0;
    P3 pos{};
    std::string name, command;
    uint32_t mask = 0;
    uint8_t signal = 0, waitUse = 0, waitHit = 0;
    std::vector<float> radii;            // DETECTOR_INRANGE's
    bool counted = false;                // the player within one of them, and of a type it counts
    P3 usePos{};                         // its use point: the place (the world's),
    float useReach = 0;                  // the horizontal reach (the record keeps it squared)
    uint32_t useId = 0;                  // the action's id: its text is the Action menu's line (0: none)
    std::string useText;
    std::vector<std::vector<P3>> boxes;  // 8 corners each
};
struct Area {
    uintptr_t frame = 0;
    std::string name;
    std::vector<P3> verts;
    std::vector<uint16_t> faces;
    std::vector<std::pair<uint16_t, uint16_t>> edges;   // the mesh's outline: not the diagonals inside a flat side
    bool inside = false, scripted = false;
    P3 lo{}, hi{};                                      // its box in the world
};
// A distance a script tests: GETACTORSDIST a, b, f, then an IFFLTINRANGE or an IF on flt[f] a few lines on (game.h
// ScriptDistParams and the rest: each command's params read in both builds' handlers). What pet1kk's developer tools
// (PR 2) showed first: the line between the two actors, the threshold around the one that is not the player.
struct DistTest {
    std::string owner;           // the name of the actor whose script it is
    P3 a{}, b{};                 // act[a]'s and act[b]'s places now
    bool aPlayer = false, bPlayer = false;
    float dist = 0;              // their distance now
    int cmp = 0;                 // 0 not compared near it, 1 dist < hi, 2 dist > lo, 3 lo <= dist < hi
    float lo = 0, hi = 0;
    bool met = false;            // the comparison holds now
    bool active = false;         // the script is at those lines now
};
std::vector<Detector> g_dets;
std::vector<DistTest> g_dists;
std::vector<Area> g_areas;
DWORD g_areasAt;                         // when the scene was last walked for areas
size_t g_framesWalked;

// A program's instructions: {opcode, params*}.
struct Instr { int op; uintptr_t params; };
std::vector<Instr> Program(uintptr_t prog) {
    std::vector<Instr> v;
    uintptr_t b = R<uintptr_t>(prog + game::kProgBegin), e = R<uintptr_t>(prog + game::kProgEnd);
    if (!b || e <= b || e - b > 8 * 20000) return v;
    v.resize((e - b) / 8);
    if (!H->mem_read(b, v.data(), v.size() * 8)) v.clear();
    return v;
}

void ReadDetector(uintptr_t a, Detector *d, int opInrange, P3 player, uint32_t playerType) {
    d->actor = a;
    d->frame = R<uintptr_t>(a + game::kActorFrame);
    if (!FrameWorldPos(d->frame, &d->pos)) d->pos = V3At(a + game::kActorPos);
    d->name = game::TextUtf8(Str(R<uintptr_t>(d->frame + game::kFrameName)).c_str());
    d->mask = R<uint32_t>(a + game::kDetInrangeMask);
    d->signal = R<uint8_t>(a + game::kDetSignal);
    d->waitUse = R<uint8_t>(a + game::kDetWaitUse);
    d->waitHit = R<uint8_t>(a + game::kDetWaitHit);
    uintptr_t prog = a + game::kDetProgram;
    std::vector<Instr> ins = Program(prog);
    int pc = R<int>(prog + game::kProgPc);
    if (ins.empty()) {
        d->command = "no script";
    } else if (pc >= 0 && pc < static_cast<int>(ins.size())) {
        const char *n = game::ScriptCommandName(ins[pc].op);
        char b[32];
        snprintf(b, sizeof b, "#%d", ins[pc].op);
        d->command = n ? n : b;
    } else {
        d->command = "its script ended";
    }
    for (const Instr &i : ins) {
        if (i.op == opInrange && i.params) {
            float r = R<float>(i.params + 4);   // {flt[] index of the count, the radius}
            bool seen = false;
            for (float x : d->radii) seen = seen || std::fabs(x - r) < 0.01f;
            if (r > 0 && r < 10000 && !seen) d->radii.push_back(r);
        }
    }
    float dist = Len(player - d->pos);
    for (float r : d->radii) if (dist < r && (d->mask & playerType)) d->counted = true;
    if (d->waitUse) {
        const uintptr_t use = a + game::kDetUse;
        P3 p = V3At(use);
        float m[16];
        if (uintptr_t mat = R<uintptr_t>(use + game::kUseMatrix); mat && H->mem_read(mat, m, sizeof m))   // (its frame's space)
            p = {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
                 p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
        d->usePos = p;
        float reach2 = R<float>(use + game::kUseReach2);
        d->useReach = reach2 > 0 && reach2 < 10000 ? std::sqrt(reach2) : 0;
        d->useId = R<uint32_t>(use + game::kUseText);
        if (d->useId) d->useText = game::TextUtf8(GameText(d->useId));
    }
    int n = R<int>(a + game::kDetVolumeCount);
    uintptr_t vols = R<uintptr_t>(a + game::kDetVolumes), worlds = R<uintptr_t>(a + game::kDetVolumeWorld);
    for (int k = 0; vols && worlds && k < n && k < 64; k++) {
        uintptr_t rec = R<uintptr_t>(vols + 4 * k);
        float box[6], m[16];
        if (!rec || !H->mem_read(rec + game::kDetVolumeBox, box, sizeof box) || !H->mem_read(worlds + 0x40 * k, m, sizeof m)) continue;
        std::vector<P3> c;
        for (int j = 0; j < 8; j++) {
            float x = box[(j & 1) ? 3 : 0], y = box[(j & 2) ? 4 : 1], z = box[(j & 4) ? 5 : 2];
            c.push_back({x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13], x * m[2] + y * m[6] + z * m[10] + m[14]});
        }
        d->boxes.push_back(c);
    }
}

// A program's distance tests (DistTest), those within `range` of the player.
void ReadDistTests(uintptr_t prog, const std::string &owner, const P3 &pp, float range) {
    static const int opDist = game::ScriptOpcode("getactorsdist"), opRange = game::ScriptOpcode("iffltinrange"), opIf = game::ScriptOpcode("if");
    if (opDist < 0) return;
    const std::vector<Instr> ins = Program(prog);
    if (ins.empty()) return;
    const int pc = R<int>(prog + game::kProgPc);
    const uintptr_t flts = R<uintptr_t>(prog + game::kProgFlts), acts = R<uintptr_t>(prog + game::kProgActs);
    const int nf = R<int>(prog + game::kProgFltCount), na = R<int>(prog + game::kProgActCount);
    auto flt = [&](int k) { return k >= 0 && k < nf && nf < 10000 ? R<float>(flts + 4 * k) : 0.0f; };
    const size_t first = g_dists.size();
    for (size_t i = 0; i < ins.size(); i++) {
        if (ins[i].op != opDist || !ins[i].params) continue;
        const game::ScriptDistParams dp = R<game::ScriptDistParams>(ins[i].params);
        if (dp.a < 0 || dp.b < 0 || dp.a >= na || dp.b >= na || na > 10000) continue;
        const uintptr_t A = R<uintptr_t>(acts + 4 * dp.a), B = R<uintptr_t>(acts + 4 * dp.b);
        DistTest t;
        t.owner = owner;
        if (!A || !B || !FrameWorldPos(R<uintptr_t>(A + game::kActorFrame), &t.a) || !FrameWorldPos(R<uintptr_t>(B + game::kActorFrame), &t.b)) continue;
        t.aPlayer = R<int>(A + game::kActorType) == 2;
        t.bPlayer = R<int>(B + game::kActorType) == 2;
        t.dist = Len(t.a - t.b);
        size_t end = i;
        for (size_t j = i + 1; j < ins.size() && j <= i + 16; j++) {
            if (!ins[j].params) continue;
            if (ins[j].op == opDist && R<game::ScriptDistParams>(ins[j].params).f == dp.f) break;   // flt[f] made anew
            if (ins[j].op == opRange) {
                const game::ScriptRangeParams r = R<game::ScriptRangeParams>(ins[j].params);
                if (r.f != dp.f) continue;
                t.cmp = 3, t.lo = r.lo, t.hi = r.hi, end = j;
                break;
            }
            if (ins[j].op == opIf) {
                const game::ScriptIfParams q = R<game::ScriptIfParams>(ins[j].params);
                if (q.cmp != game::kScriptIfLess && q.cmp != game::kScriptIfGreater) continue;
                const bool left = q.left == dp.f, right = q.right == dp.f;
                if (left == right) continue;
                const float other = left ? (q.right < 0 ? q.rightImm : flt(q.right)) : (q.left < 0 ? q.leftImm : flt(q.left));
                const bool less = left == (q.cmp == game::kScriptIfLess);   // "dist < x", or "x > dist"
                t.cmp = less ? 1 : 2;
                (less ? t.hi : t.lo) = other;
                end = j;
                break;
            }
        }
        t.met = t.cmp == 1 ? t.dist < t.hi : t.cmp == 2 ? t.dist > t.lo : t.cmp == 3 ? t.dist >= t.lo && t.dist < t.hi : false;
        t.active = pc >= static_cast<int>(i) - 2 && pc <= static_cast<int>(end) + 12;
        if ((std::min)(Len(t.a - pp), Len(t.b - pp)) > range) continue;
        bool seen = false;   // (a script asks the same pair in several places: one test a comparison)
        for (size_t k = first; k < g_dists.size() && !seen; k++) {
            const DistTest &o = g_dists[k];
            seen = o.cmp == t.cmp && o.lo == t.lo && o.hi == t.hi && Len(o.a - t.a) < 0.01f && Len(o.b - t.b) < 0.01f;
            if (seen && t.active) g_dists[k].active = true;
        }
        if (!seen) g_dists.push_back(t);
    }
}

// The areas of the scene: its frames of type 14 (walked again every few seconds: a mission's scene keeps them).
void FindAreas() {
    g_areas.clear();
    void *player = game::Player();
    uintptr_t root = player ? R<uintptr_t>(reinterpret_cast<uintptr_t>(player) + game::kActorFrame) : 0;
    for (int i = 0; i < 32 && root && R<uintptr_t>(root + game::kFrameParent); i++) root = R<uintptr_t>(root + game::kFrameParent);
    if (!root) return;
    std::vector<void *> frames;
    if (!SafeEnumDeep(reinterpret_cast<void *>(root), &frames)) frames.clear();
    g_framesWalked = frames.size();
    for (void *f : frames) {
        uintptr_t fr = reinterpret_cast<uintptr_t>(f);
        if (R<uint32_t>(fr + game::kFrameType) != game::kFrameTypeArea) continue;
        Area a;
        a.frame = fr;
        a.name = game::TextUtf8(Str(R<uintptr_t>(fr + game::kFrameName)).c_str());
        g_areas.push_back(a);
    }
}

void ReadArea(Area *a, P3 player) {
    int nv = R<int>(a->frame + game::kAreaVertCount), nf = R<int>(a->frame + game::kAreaFaceCount);
    a->verts.clear();
    a->faces.clear();
    if (nv > 0 && nv < 4096) {
        a->verts.resize(nv);
        if (!H->mem_read(R<uintptr_t>(a->frame + game::kAreaVertsWorld), a->verts.data(), nv * sizeof(P3))) a->verts.clear();
    }
    if (nf > 0 && nf < 8192) {
        a->faces.resize(nf * 3);
        if (!H->mem_read(R<uintptr_t>(a->frame + game::kAreaFaces), a->faces.data(), nf * 6)) a->faces.clear();
    }
    // the edges drawn: each once, and not one between two triangles of the same plane (a box's sides are two
    // triangles each: their diagonal is no edge of the volume)
    a->edges.clear();
    struct E { uint16_t i, j; P3 n; int faces; bool flat; };
    std::vector<E> es;
    auto normal = [&](size_t f) {
        P3 p = a->verts[a->faces[f]], q = a->verts[a->faces[f + 1]], r = a->verts[a->faces[f + 2]];
        P3 u = q - p, v = r - p, n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        float l = Len(n);
        return l > 1e-6f ? n * (1.0f / l) : n;
    };
    for (size_t f = 0; f + 2 < a->faces.size(); f += 3) {
        if (a->faces[f] >= a->verts.size() || a->faces[f + 1] >= a->verts.size() || a->faces[f + 2] >= a->verts.size()) continue;
        P3 n = normal(f);
        for (int k = 0; k < 3; k++) {
            uint16_t i = a->faces[f + k], j = a->faces[f + (k + 1) % 3];
            if (i > j) std::swap(i, j);
            E *found = nullptr;
            for (E &e : es) if (e.i == i && e.j == j) { found = &e; break; }
            if (!found) { es.push_back({i, j, n, 1, false}); continue; }
            found->faces++;
            found->flat = n.x * found->n.x + n.y * found->n.y + n.z * found->n.z > 0.999f;
        }
    }
    for (const E &e : es) if (!(e.faces == 2 && e.flat)) a->edges.push_back({e.i, e.j});
    if (!a->verts.empty()) {
        a->lo = a->hi = a->verts[0];
        for (const P3 &v : a->verts) {
            a->lo = {(std::min)(a->lo.x, v.x), (std::min)(a->lo.y, v.y), (std::min)(a->lo.z, v.z)};
            a->hi = {(std::max)(a->hi.x, v.x), (std::max)(a->hi.y, v.y), (std::max)(a->hi.z, v.z)};
        }
    }
    a->inside = AreaCheck(a->frame, player) == 1;
}

// Everything read again (every frame: a script moves on; the scene's areas every 3 s).
void Refresh() {
    g_dets.clear();
    g_dists.clear();
    if (!g_game || H->mission_state() == MML_MISSION_NONE) { g_areas.clear(); return; }
    void *player = game::Player();
    P3 pp{};
    if (!player || !FrameWorldPos(R<uintptr_t>(reinterpret_cast<uintptr_t>(player) + game::kActorFrame), &pp)) return;
    uint32_t ptype = R<uint32_t>(reinterpret_cast<uintptr_t>(player) + game::kHumanType);
    float range = static_cast<float>(H->config_number(M, SECTION "range", 150));
    int opInrange = game::ScriptOpcode("detector_inrange"), opArea = game::ScriptOpcode("ispointinarea");
    std::vector<void *> actors;
    if (!SafeListActors(&actors)) actors.clear();
    if (GetTickCount() - g_areasAt > 3000) { FindAreas(); g_areasAt = GetTickCount(); }
    for (Area &a : g_areas) a.scripted = false;
    for (void *act : actors) {
        uintptr_t a = reinterpret_cast<uintptr_t>(act);
        int kind = R<int>(a + game::kActorType);
        uintptr_t prog = kind == game::kActorTypeDetector ? a + game::kDetProgram : kind == game::kActorTypeEntity ? a + game::kEntityProgram : 0;
        if (!prog) continue;
        // the areas its ISPOINTINAREA names: {flt[] of the point, frm[] of the area, flt[] of the answer}
        if (!g_areas.empty()) {
            uintptr_t frms = R<uintptr_t>(prog + game::kProgFrames);
            int nfr = R<int>(prog + game::kProgFrameCount);
            for (const Instr &i : Program(prog)) {
                if (i.op != opArea || !i.params) continue;
                int k = R<int>(i.params + 4);
                uintptr_t f = k >= 0 && k < nfr ? R<uintptr_t>(frms + 4 * k) : 0;
                for (Area &ar : g_areas) if (ar.frame == f) ar.scripted = true;
            }
        }
        if (H->config_number(M, SECTION "distances", 1) != 0)
            ReadDistTests(prog, game::TextUtf8(Str(R<uintptr_t>(R<uintptr_t>(a + game::kActorFrame) + game::kFrameName)).c_str()), pp, range);
        if (kind != game::kActorTypeDetector) continue;
        Detector d;
        ReadDetector(a, &d, opInrange, pp, ptype);
        if (Len(d.pos - pp) <= range) g_dets.push_back(d);
    }
    for (Area &a : g_areas) ReadArea(&a, pp);
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
// A line in the world: one line on the screen (a straight line stays straight in perspective), its part behind the
// camera's near plane cut off first (a line passing behind the eye would be thrown across the screen). One item a
// line: the frame holds 4096 of them for every mod together.
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
// The nearest point of a box (lo, hi) to p, its distance.
float BoxDistance(const P3 &lo, const P3 &hi, const P3 &p) {
    P3 q{(std::max)(lo.x, (std::min)(p.x, hi.x)), (std::max)(lo.y, (std::min)(p.y, hi.y)), (std::max)(lo.z, (std::min)(p.z, hi.z))};
    return Len(q - p);
}
// As many pieces as a circle of radius r at c needs to look round from the camera: 8 far or small, 32 near.
int Segments(const P3 &c, float r) {
    float d = (std::max)(Len(c - g_eye), 1.0f);
    return (std::max)(8, (std::min)(32, static_cast<int>(8 + 48 * r / d)));
}
void Circle(const P3 &c, float r, int plane, uint32_t argb) {
    const int segs = Segments(c, r);
    P3 prev{};
    for (int k = 0; k <= segs; k++) {
        float t = 6.2831853f * k / segs, u = std::cos(t) * r, v = std::sin(t) * r;
        P3 p = plane == 0 ? c + P3{u, 0, v} : plane == 1 ? c + P3{u, v, 0} : c + P3{0, v, u};
        if (k) Line3(prev, p, argb);
        prev = p;
    }
}
void Sphere(const P3 &c, float r, uint32_t argb) {
    Circle(c, r, 0, argb);
    Circle(c, r, 1, argb);
    Circle(c, r, 2, argb);
}
// A use point's reach: an upright cylinder, its radius r, from c.y - h to c.y + h.
void Cylinder(const P3 &c, float r, float h, uint32_t argb) {
    Circle(c - P3{0, h, 0}, r, 0, argb);
    Circle(c + P3{0, h, 0}, r, 0, argb);
    for (int k = 0; k < 4; k++) {
        float t = 1.5707963f * k;
        P3 p = c + P3{std::cos(t) * r, 0, std::sin(t) * r};
        Line3(p - P3{0, h, 0}, p + P3{0, h, 0}, argb);
    }
}
// A label above a place (lines.h: kept on the screen and off every view's labels drawn before it this frame).
void Label(const P3 &w, const std::vector<std::string> &text, uint32_t argb) { lines::Label(H, M, &w.x, text, argb); }

void DrawDetectors(const P3 &pp, float labels);

void OnFrame(const MmlEvent *, void *) {
    // (its lines and labels are 2D, a layer of the HUD's: the game's menus are drawn over them)
    if (!g_on || !g_game || H->mission_state() == MML_MISSION_NONE) return;
    Refresh();
    void *player = game::Player();
    P3 pp{};
    if (!player || !FrameWorldPos(R<uintptr_t>(reinterpret_cast<uintptr_t>(player) + game::kActorFrame), &pp)) return;
    ReadCamera();
    const float labels = static_cast<float>(H->config_number(M, SECTION "label_range", 50));
    // the detectors first: what they draw is near; the areas (a district is hundreds of metres) after them
    if (H->config_number(M, SECTION "detectors", 1) != 0) DrawDetectors(pp, labels);
    // the distances the scripts test: the line between the two actors, the threshold around the one that is not the
    // player; green while the comparison holds, yellow while the script is at it, dim otherwise
    for (const DistTest &t : g_dists) {
        const uint32_t col = t.met ? 0xFF40FF60u : t.active ? 0xFFFFD040u : 0x80A0A0A0u;
        Line3(t.a, t.b, col);
        const P3 c = t.aPlayer && !t.bPlayer ? t.b : t.a;
        const float r = t.cmp == 2 ? t.lo : t.hi;
        if (t.cmp && r > 0 && r < 5000) Sphere(c, r, (col & 0x00FFFFFFu) | (t.active || t.met ? 0xA0000000u : 0x50000000u));
        if (t.cmp == 3 && t.lo > 0 && t.lo < t.hi) Sphere(c, t.lo, (col & 0x00FFFFFFu) | 0x50000000u);
        if (Len(c - pp) > labels) continue;
        char b[96];
        if (t.cmp == 1) snprintf(b, sizeof b, "distance %.1f < %.1f", t.dist, t.hi);
        else if (t.cmp == 2) snprintf(b, sizeof b, "distance %.1f > %.1f", t.dist, t.lo);
        else if (t.cmp == 3) snprintf(b, sizeof b, "distance %.1f in %.1f..%.1f", t.dist, t.lo, t.hi);
        else snprintf(b, sizeof b, "distance %.1f (compared further on)", t.dist);
        Label(c + P3{0, 2.2f, 0}, {t.owner + ": getactorsdist", std::string(b) + (t.met ? ": holds" : "")}, col);
    }
    if (H->config_number(M, SECTION "areas", 1) == 0) return;
    const float range = static_cast<float>(H->config_number(M, SECTION "range", 150));
    for (const Area &a : g_areas) {
        if (a.verts.empty() || (!a.inside && BoxDistance(a.lo, a.hi, pp) > range)) continue;
        uint32_t col = a.inside ? 0xFF40FF60u : a.scripted ? 0xFFFF40FFu : 0x90C040C0u;
        for (const auto &e : a.edges) Line3(a.verts[e.first], a.verts[e.second], col);
        P3 c{};
        for (const P3 &v : a.verts) c = c + v;
        c = c * (1.0f / a.verts.size());
        if (Len(c - pp) < labels * 2)
            Label(c, {"area " + a.name, a.inside ? "the player is inside" : a.scripted ? "tested by a script" : "not tested now"}, col);
    }
}

void DrawDetectors(const P3 &pp, float labels) {
    for (const Detector &d : g_dets) {
        // a detector whose script has ended tests nothing any more: dim, its label only close by
        const bool ended = d.command == "its script ended" || d.command == "no script";
        uint32_t col = d.counted ? 0xFF40FF60u : ended ? 0xA0A0A0A0u : 0xFFFFD040u;
        if (H->config_number(M, SECTION "inrange", 1) != 0)
            for (float r : d.radii) Sphere(d.pos, r, ended ? 0x40A0A0A0u : d.counted ? 0xC040FF60u : 0x80FFD040u);
        // where the Action key finds it: the point 0.8 m before the player and 0.9 m up within this cylinder
        if (d.waitUse && d.useReach > 0 && H->config_number(M, SECTION "use", 1) != 0) Cylinder(d.usePos, d.useReach, game::kUseHeight, 0xC040E0FFu);
        if (H->config_number(M, SECTION "boxes", 1) != 0)
            for (const std::vector<P3> &c : d.boxes) {
                static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto &k : e) Line3(c[k[0]], c[k[1]], 0xFFFF8030u);
            }
        // its place: a small cross
        Line3(d.pos - P3{0.3f, 0, 0}, d.pos + P3{0.3f, 0, 0}, col);
        Line3(d.pos - P3{0, 0, 0.3f}, d.pos + P3{0, 0, 0.3f}, col);
        Line3(d.pos, d.pos + P3{0, 1.2f, 0}, col);
        if (Len(d.pos - pp) > (ended ? (std::min)(labels, 8.0f) : labels)) continue;
        std::vector<std::string> lines = {d.name};
        char b[160];
        snprintf(b, sizeof b, "%s%s%s, signal %d", d.command.empty() ? "no script" : d.command.c_str(),
                 d.waitUse ? " (use)" : "", d.waitHit ? " (hit)" : "", d.signal);
        lines.push_back(b);
        for (float r : d.radii) {
            snprintf(b, sizeof b, "in range %.1f m of types 0x%X%s", r, d.mask, d.counted ? ": the player counted" : "");
            lines.push_back(b);
        }
        if (d.waitUse) {
            snprintf(b, sizeof b, "used within %.2f m", d.useReach);
            lines.push_back(d.useText.empty() ? std::string(b) : std::string(b) + ": \"" + d.useText + "\"");
        }
        if (!d.boxes.empty()) { snprintf(b, sizeof b, "%zu box(es) to hit", d.boxes.size()); lines.push_back(b); }
        Label(d.pos + P3{0, 1.4f, 0}, lines, col);
    }
}

void ReadSettings() {
    g_on = H->config_number(M, SECTION "show", 1) != 0;
    H->hotkey(M, H->config_string(M, SECTION "hotkey", "F7"), [](void *) { g_on = !g_on; H->config_set(M, SECTION "show", g_on ? "true" : "false"); }, nullptr);
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_LOADED || ev->type == MML_EVENT_MISSION_CLOSE) { g_areas.clear(); g_areasAt = 0; }
}

// debug_views.triggers.list: what is drawn, as data
int List(const char *, MmlReply *reply, void *) {
    if (!g_game) { H->reply_append(reply, "not a known build of the game", static_cast<size_t>(-1)); return 1; }
    Refresh();
    std::string out = "{\"mission\":\"" + Esc(H->mission_name()) + "\",\"detectors\":[";
    char b[512];
    for (size_t k = 0; k < g_dets.size(); k++) {
        const Detector &d = g_dets[k];
        snprintf(b, sizeof b, "%s{\"actor\":\"0x%08X\",\"name\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],\"command\":\"%s\",\"signal\":%d,"
                 "\"wait_use\":%d,\"wait_hit\":%d,\"mask\":%u,\"counted\":%s,\"boxes\":%zu,\"radii\":[",
                 k ? "," : "", static_cast<unsigned>(d.actor), Esc(d.name).c_str(), d.pos.x, d.pos.y, d.pos.z, Esc(d.command).c_str(), d.signal,
                 d.waitUse, d.waitHit, d.mask, d.counted ? "true" : "false", d.boxes.size());
        out += b;
        for (size_t r = 0; r < d.radii.size(); r++) { snprintf(b, sizeof b, "%s%.2f", r ? "," : "", d.radii[r]); out += b; }
        out += "]";
        if (d.waitUse) {
            snprintf(b, sizeof b, ",\"use\":{\"pos\":[%.2f,%.2f,%.2f],\"reach\":%.2f,\"height\":%.2f,\"text_id\":%u,\"text\":\"",
                     d.usePos.x, d.usePos.y, d.usePos.z, d.useReach, game::kUseHeight, d.useId);
            out += b + Esc(d.useText) + "\"}";
        }
        for (size_t k2 = 0; k2 < d.boxes.size(); k2++) {   // each box by two opposite corners
            const P3 &lo = d.boxes[k2][0], &hi = d.boxes[k2][7];
            snprintf(b, sizeof b, "%s[[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f]]", k2 ? "," : ",\"box_corners\":[", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
            out += b;
            if (k2 + 1 == d.boxes.size()) out += "]";
        }
        out += "}";
    }
    out += "],\"areas\":[";
    for (size_t k = 0; k < g_areas.size(); k++) {
        const Area &a = g_areas[k];
        snprintf(b, sizeof b, "%s{\"frame\":\"0x%08X\",\"name\":\"%s\",\"vertices\":%zu,\"triangles\":%zu,\"inside\":%s,\"scripted\":%s%s",
                 k ? "," : "", static_cast<unsigned>(a.frame), Esc(a.name).c_str(), a.verts.size(), a.faces.size() / 3,
                 a.inside ? "true" : "false", a.scripted ? "true" : "false", a.verts.empty() ? "" : ",\"first\":");
        out += b;
        if (!a.verts.empty()) { snprintf(b, sizeof b, "[%.2f,%.2f,%.2f]", a.verts[0].x, a.verts[0].y, a.verts[0].z); out += b; }
        out += "}";
    }
    snprintf(b, sizeof b, "],\"frames_walked\":%zu}", g_framesWalked);
    out += b;
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}
} // namespace

int TriggersLoad(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, camera_project) || !MML_HAS(host, frame_world)) return 1;   // a loader older than this mod
    g_game = game::Detect(host->module_base("LS3DF.dll"));
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_LOADED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->command(mod, "triggers.list", "the scripts' triggers near the player: detectors (place, command, signal, radii, use point, boxes) and the scene's areas", List, nullptr);
    host->log(mod, MML_LOG_INFO, "triggers loaded (game %s)", host->game_build());
    return 0;
}

void TriggersUnload() {}
