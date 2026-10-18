// flow_smoke: a mod in two halves (docs/MODDING.md "64-bit halves") doing what a 32-bit game cannot: NVIDIA Flow 2,
// smoke and fire as gas on a sparse grid, simulated and ray-marched on the GPU (Vulkan) in the 64-bit half
// (flow_smoke64.cpp), in the game's world, in the same frame. This 32-bit half keeps the sources, gives the 64-bit half
// the world the gas meets as the game draws it (the scenery's meshes around the sources, each car's own meshes posed
// now, each person's body as capsules along his bones), and in its graphics pass hands Flow this frame (its
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
//   call flow_smoke.move  {"id": 2, "at": [x, y, z]}  the newest fire (or that source) moved where the crosshair looks
//   F5: the newest fire moved where the crosshair looks (none: one made there); F10: fire in front of the player
//   (again: one more); Shift+F10: everything cleared
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
MmlChannel *g_frameCh, *g_viewCh, *g_doneCh, *g_sceneCh, *g_moverCh;
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
    uint64_t frames, asked, drawn, late, none, noTextures, silent, meshFrames;
    double rtSum, rtMax, prepSum, prepMax, meshSum, meshMax;
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

// ---- the frame's state out: the sources where they are now, the scenery and the movers near them, as meshes
fsm::Frame g_rec;
std::vector<MmlShape> g_shapes(512);
std::vector<MmlMeshPiece> g_pieces(4096);
std::vector<uint8_t> g_sceneBuf(fsm::kSceneryBytes), g_moverBuf(fsm::kMoversBytes);
uint64_t g_sceneSig;
uint32_t g_sceneSerial, g_sceneTriangles, g_sceneVertices;
float g_focusLo[3], g_focusHi[3];
bool g_haveFocus;
struct BonesLast {
    float p[MML_BONE_COUNT][3];
    uint64_t frame;
};
std::map<const void *, BonesLast> g_bonesLast;   // a person's bones' last places (their velocities), by the actor
fsm::Movers g_moverRec;

uint64_t Fnv(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ull;
    return h;
}
// p * m (row vectors), about the origin
void ToOrigin(const float *p, const float *m, float *out) {
    for (int k = 0; k < 3; k++)
        out[k] = float(double(p[0]) * m[k] + double(p[1]) * m[4 + k] + double(p[2]) * m[8 + k] + m[12 + k] - g_origin[k]);
}

// The static world the game draws around the sources, in one soup of triangles: written when the scan's pieces or
// their versions change (a new box, a door moved), else left as it is. Additive light (lamps' glows) is not solid.
void Scenery(const float lo[3], const float hi[3]) {
    float want[2][3] = {{lo[0] - 12, float(g_ground + g_origin[1]) - 3, lo[2] - 12}, {hi[0] + 12, hi[1] + 20, hi[2] + 12}};
    bool move = !g_haveFocus;
    for (int k = 0; k < 3 && !move; k++) move = std::fabs(want[0][k] - g_focusLo[k]) > 4 || std::fabs(want[1][k] - g_focusHi[k]) > 4;
    if (move) {
        memcpy(g_focusLo, want[0], sizeof g_focusLo);
        memcpy(g_focusHi, want[1], sizeof g_focusHi);
        H->scene_focus(M, g_focusLo, g_focusHi);
        g_haveFocus = true;
    }
    int n = std::min(H->scene_pieces(M, g_pieces.data(), int(g_pieces.size())), int(g_pieces.size()));
    // (the set of pieces and their versions, whatever order the scan gives them in)
    uint64_t sig = Fnv(1469598103934665603ull, uint64_t(g_clear) * 0x9E3779B97F4A7C15ull + uint64_t(n));
    for (int i = 0; i < n; i++) sig += Fnv(Fnv(1469598103934665603ull, uint64_t(uintptr_t(g_pieces[i].visual))), g_pieces[i].version);
    if (sig == g_sceneSig) return;
    g_sceneSig = sig;
    auto *hdr = reinterpret_cast<fsm::Scenery *>(g_sceneBuf.data());
    float *pos = reinterpret_cast<float *>(hdr + 1);
    std::vector<uint32_t> idx;
    idx.reserve(3 * 65536);
    uint32_t verts = 0;
    for (int i = 0; i < n; i++) {
        const MmlMeshPiece &pc = g_pieces[i];
        if (!pc.pos || !pc.index || pc.vertices <= 0 || verts + uint32_t(pc.vertices) > fsm::kMaxSceneVertices) continue;
        for (int v = 0; v < pc.vertices; v++) ToOrigin(pc.pos + 3 * v, pc.world, pos + 3 * (verts + v));
        for (int t = 0; t < pc.triangles && idx.size() / 3 < fsm::kMaxSceneTriangles; t++) {
            if (pc.material && (pc.material[t] & 0x80000000u)) continue;
            const uint32_t *tri = pc.index + 3 * t;
            if (tri[0] >= uint32_t(pc.vertices) || tri[1] >= uint32_t(pc.vertices) || tri[2] >= uint32_t(pc.vertices)) continue;
            for (int k = 0; k < 3; k++) idx.push_back(verts + tri[k]);
        }
        verts += pc.vertices;
    }
    hdr->serial = ++g_sceneSerial;
    hdr->vertices = verts;
    hdr->triangles = uint32_t(idx.size() / 3);
    hdr->pieces = uint32_t(n);
    // (the indices right after the positions written)
    memmove(pos + 3 * verts, idx.data(), idx.size() * 4);
    H->channel_write(g_sceneCh, g_sceneBuf.data(), uint32_t(sizeof *hdr + 12 * verts + 4 * idx.size()));
    g_sceneTriangles = hdr->triangles, g_sceneVertices = verts;
}

// A capsule's surface from a to b of radius r (8 sides, a ring at each end and a tip beyond each), its vertices'
// velocities from va to vb: 18 vertices, 32 triangles.
const int kCapsuleVerts = 18, kCapsuleTris = 32;
void Capsule(const float a[3], const float b[3], float r, const float va[3], const float vb[3], float *pos, float *vel, uint32_t *idx,
             uint32_t base) {
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float u[3] = {0, 1, 0};
    if (len > 1e-4f) u[0] = d[0] / len, u[1] = d[1] / len, u[2] = d[2] / len;
    float s[3] = {std::fabs(u[1]) < 0.9f ? 0.f : 1.f, std::fabs(u[1]) < 0.9f ? 1.f : 0.f, 0};   // (any axis not along u)
    float x[3] = {u[1] * s[2] - u[2] * s[1], u[2] * s[0] - u[0] * s[2], u[0] * s[1] - u[1] * s[0]};
    float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (float &c : x) c /= xl;
    float y[3] = {u[1] * x[2] - u[2] * x[1], u[2] * x[0] - u[0] * x[2], u[0] * x[1] - u[1] * x[0]};
    for (int ring = 0; ring < 2; ring++)
        for (int k = 0; k < 8; k++) {
            float ang = k * 0.785398163f, c = std::cos(ang) * r, sn = std::sin(ang) * r;
            const float *e = ring ? b : a;
            const float *ve = ring ? vb : va;
            float *p = pos + 3 * (ring * 8 + k), *v = vel + 3 * (ring * 8 + k);
            for (int j = 0; j < 3; j++) p[j] = e[j] + x[j] * c + y[j] * sn, v[j] = ve[j];
        }
    for (int j = 0; j < 3; j++) {
        pos[48 + j] = a[j] - u[j] * r, vel[48 + j] = va[j];
        pos[51 + j] = b[j] + u[j] * r, vel[51 + j] = vb[j];
    }
    uint32_t *t = idx;
    for (int k = 0; k < 8; k++) {
        uint32_t k1 = (k + 1) % 8;
        uint32_t a0 = base + k, a1 = base + k1, b0 = base + 8 + k, b1 = base + 8 + k1;
        // (wound as the game's meshes are, Direct3D's front faces: clockwise seen from outside, so that Flow's normal,
        // (v2 - v1) x (v3 - v1), points out and what is behind it is inside)
        *t++ = a0, *t++ = a1, *t++ = b0;   // (the side)
        *t++ = a1, *t++ = b1, *t++ = b0;
        *t++ = base + 16, *t++ = a1, *t++ = a0;   // (the end at a)
        *t++ = base + 17, *t++ = b0, *t++ = b1;   // (the end at b)
    }
}

// What moves around the sources, as meshes: each car's own (its pieces posed now; each vertex's velocity the body's,
// v + turn x r), each person's body made of capsules along his bones (each bone's velocity from its last place).
void Movers(const float lo[3], const float hi[3], float dt) {
    fsm::Movers &rec = g_moverRec;
    rec = fsm::Movers{};
    rec.frame = g_frame;
    float rlo[3] = {lo[0] - 30, float(g_ground + g_origin[1]) - 3, lo[2] - 30}, rhi[3] = {hi[0] + 30, hi[1] + 25, hi[2] + 30};
    int n = std::min(H->world_shapes(M, rlo, rhi, MML_WORLD_MOVING, g_shapes.data(), int(g_shapes.size())), int(g_shapes.size()));
    std::vector<std::pair<void *, uint32_t>> actors;   // (the actor, kCar or kPerson), each once
    for (int i = 0; i < n; i++) {
        const MmlShape &sh = g_shapes[i];
        uint32_t kind = (sh.classes & 0x200) ? fsm::kCar : (sh.classes & 0x100) ? fsm::kPerson : 0;
        if (!kind || !sh.actor) continue;
        if (std::find_if(actors.begin(), actors.end(), [&](const auto &a) { return a.first == sh.actor; }) == actors.end())
            actors.push_back({sh.actor, kind});
    }
    float *pos = reinterpret_cast<float *>(g_moverBuf.data() + sizeof(fsm::Movers));
    float *vel = pos + 3 * fsm::kMaxMoverVertices;
    uint32_t *idx = reinterpret_cast<uint32_t *>(vel + 3 * fsm::kMaxMoverVertices);
    MmlBone bones[MML_BONE_COUNT];
    for (const auto &a : actors) {
        if (rec.count >= fsm::kMaxMovers) break;
        fsm::Mover m{};
        m.kind = a.second;
        m.id = uint64_t(uintptr_t(a.first));
        m.firstVertex = rec.vertices, m.firstTriangle = rec.triangles;
        if (a.second == fsm::kCar) {
            MmlVehicleBody body;
            if (!CarBody(a.first, &body)) continue;
            int pn = std::min(H->actor_meshes(M, a.first, g_pieces.data(), int(g_pieces.size())), int(g_pieces.size()));
            uint64_t topo = 1469598103934665603ull;
            for (int i = 0; i < pn; i++) {
                const MmlMeshPiece &pc = g_pieces[i];
                if (!pc.pos || !pc.index || pc.vertices <= 0) continue;
                if (rec.vertices + m.vertices + pc.vertices > fsm::kMaxMoverVertices ||
                    rec.triangles + m.triangles + pc.triangles > fsm::kMaxMoverTriangles)
                    break;
                topo = Fnv(Fnv(topo, uint64_t(uintptr_t(pc.visual))), uint64_t(pc.triangles));
                uint32_t base = rec.vertices + m.vertices;
                for (int v = 0; v < pc.vertices; v++) {
                    float *p = pos + 3 * (base + v), *w = vel + 3 * (base + v);
                    ToOrigin(pc.pos + 3 * v, pc.world, p);
                    float r[3] = {float(p[0] + g_origin[0] - body.pos[0]), float(p[1] + g_origin[1] - body.pos[1]), float(p[2] + g_origin[2] - body.pos[2])};
                    w[0] = body.velocity[0] + body.turn[1] * r[2] - body.turn[2] * r[1];
                    w[1] = body.velocity[1] + body.turn[2] * r[0] - body.turn[0] * r[2];
                    w[2] = body.velocity[2] + body.turn[0] * r[1] - body.turn[1] * r[0];
                }
                uint32_t *t = idx + 3 * (rec.triangles + m.triangles);
                for (int k = 0; k < 3 * pc.triangles; k++) t[k] = m.vertices + std::min(pc.index[k], uint32_t(pc.vertices - 1));
                m.vertices += pc.vertices;
                m.triangles += pc.triangles;
            }
            m.topology = uint32_t(topo ^ (topo >> 32));
        } else {
            if (H->human_skeleton(M, a.first, bones, MML_BONE_COUNT) < MML_BONE_COUNT) continue;
            BonesLast &last = g_bonesLast[a.first];
            bool fresh = last.frame + 1 == g_frame && dt > 0;
            float p[MML_BONE_COUNT][3], v[MML_BONE_COUNT][3] = {};
            for (int bi = 0; bi < MML_BONE_COUNT; bi++)
                for (int k = 0; k < 3; k++) {
                    p[bi][k] = float(bones[bi].world[12 + k] - g_origin[k]);
                    if (fresh) v[bi][k] = (p[bi][k] - last.p[bi][k]) / dt;
                }
            memcpy(last.p, p, sizeof p);
            last.frame = g_frame;
            if (!bones[MML_BONE_PELVIS].frame || !bones[MML_BONE_NECK].frame) continue;
            // (the head: a capsule from the neck on, along the spine's last direction)
            float up[3] = {p[MML_BONE_NECK][0] - p[MML_BONE_BACK3][0], p[MML_BONE_NECK][1] - p[MML_BONE_BACK3][1], p[MML_BONE_NECK][2] - p[MML_BONE_BACK3][2]};
            float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            float head[3];
            for (int k = 0; k < 3; k++) head[k] = p[MML_BONE_NECK][k] + (ul > 1e-4f ? up[k] / ul : k == 1) * 0.22f;
            struct Seg { int a, b; float r; } segs[] = {
                {MML_BONE_PELVIS, MML_BONE_BACK3, 0.16f}, {MML_BONE_BACK3, MML_BONE_NECK, 0.17f},
                {MML_BONE_R_ARM, MML_BONE_R_ELBOW, 0.055f}, {MML_BONE_R_ELBOW, MML_BONE_R_HAND, 0.045f},
                {MML_BONE_L_ARM, MML_BONE_L_ELBOW, 0.055f}, {MML_BONE_L_ELBOW, MML_BONE_L_HAND, 0.045f},
                {MML_BONE_R_THIGH, MML_BONE_R_SHIN, 0.08f}, {MML_BONE_R_SHIN, MML_BONE_R_FOOT, 0.06f},
                {MML_BONE_L_THIGH, MML_BONE_L_SHIN, 0.08f}, {MML_BONE_L_SHIN, MML_BONE_L_FOOT, 0.06f}, {-1, -1, 0.11f}};
            for (const Seg &sg : segs) {
                if (rec.vertices + m.vertices + kCapsuleVerts > fsm::kMaxMoverVertices || rec.triangles + m.triangles + kCapsuleTris > fsm::kMaxMoverTriangles)
                    break;
                if (sg.a >= 0 && (!bones[sg.a].frame || !bones[sg.b].frame)) continue;
                const float *pa = sg.a >= 0 ? p[sg.a] : p[MML_BONE_NECK], *pb = sg.a >= 0 ? p[sg.b] : head;
                const float *va = sg.a >= 0 ? v[sg.a] : v[MML_BONE_NECK], *vb = sg.a >= 0 ? v[sg.b] : v[MML_BONE_NECK];
                uint32_t base = rec.vertices + m.vertices;
                uint32_t *t = idx + 3 * (rec.triangles + m.triangles);
                Capsule(pa, pb, sg.r, va, vb, pos + 3 * base, vel + 3 * base, t, m.vertices);
                m.vertices += kCapsuleVerts;
                m.triangles += kCapsuleTris;
            }
            m.topology = 1;   // (the same capsules every frame)
        }
        if (!m.vertices) continue;
        rec.mover[rec.count++] = m;
        rec.vertices += m.vertices;
        rec.triangles += m.triangles;
    }
    if (g_bonesLast.size() > 256)
        for (auto it = g_bonesLast.begin(); it != g_bonesLast.end();) it = it->second.frame + 2 < g_frame ? g_bonesLast.erase(it) : std::next(it);
    // (the header, the positions and velocities of the vertices there are, the indices after them)
    memcpy(g_moverBuf.data(), &rec, sizeof rec);
    float *velOut = pos + 3 * rec.vertices;
    memmove(velOut, vel, 12 * size_t(rec.vertices));
    uint32_t *idxOut = reinterpret_cast<uint32_t *>(velOut + 3 * rec.vertices);
    memmove(idxOut, idx, 12 * size_t(rec.triangles));
    H->channel_write(g_moverCh, g_moverBuf.data(), uint32_t(sizeof rec + 24 * size_t(rec.vertices) + 12 * size_t(rec.triangles)));
}

// The collision by the game's own shapes (the default): each person's sphere (a body's, 0.45 m about his waist), each
// car's whole hull (its box), with their velocities from their last places; the ground is the half's.
bool g_meshCollision;   // flow_smoke.collision {"mode": "meshes"}: the drawn meshes' instead (Scenery, Movers)
struct Last {
    float c[3];
    uint64_t frame;
};
std::map<const void *, Last> g_last;   // a shape's last centre (its velocity), by its primitive

float g_shapeLo[3], g_shapeHi[3];   // the box the shapes were taken from (the pass measures how far they moved since)
struct Lag {
    double sum, max;
    uint64_t n;
} g_lag;

// How far the moving cars' boxes have gone between the shapes sent to Flow and the frame's drawing (the pass): the
// gas meets them where they were then. Cars moving faster than 1 m/s only.
void MeasureLag() {
    int n = std::min(H->world_shapes(M, g_shapeLo, g_shapeHi, MML_WORLD_MOVING, g_shapes.data(), int(g_shapes.size())), int(g_shapes.size()));
    for (int i = 0; i < n; i++) {
        const MmlShape &sh = g_shapes[i];
        if (!(sh.classes & 0x200) || sh.kind != MML_SHAPE_BOX || !(sh.classes & 0x2)) continue;
        auto it = g_last.find(sh.prim);
        if (it == g_last.end() || it->second.frame != g_frame) continue;
        MmlVehicleBody b;
        if (!CarBody(sh.actor, &b) || b.velocity[0] * b.velocity[0] + b.velocity[1] * b.velocity[1] + b.velocity[2] * b.velocity[2] < 1) continue;
        double d = 0;
        for (int k = 0; k < 3; k++) d += double(sh.center[k] - it->second.c[k]) * (sh.center[k] - it->second.c[k]);
        d = std::sqrt(d);
        g_lag.sum += d, g_lag.max = std::max(g_lag.max, d), g_lag.n++;
    }
}

void Shapes(const float lo[3], const float hi[3], float dt) {
    fsm::Frame &f = g_rec;
    float rlo[3] = {lo[0] - 30, float(g_ground + g_origin[1]) - 3, lo[2] - 30}, rhi[3] = {hi[0] + 30, hi[1] + 25, hi[2] + 30};
    memcpy(g_shapeLo, rlo, sizeof rlo);
    memcpy(g_shapeHi, rhi, sizeof rhi);
    int n = std::min(H->world_shapes(M, rlo, rhi, MML_WORLD_MOVING, g_shapes.data(), int(g_shapes.size())), int(g_shapes.size()));
    for (int i = 0; i < n && f.shapes < fsm::kMaxShapes; i++) {
        const MmlShape &sh = g_shapes[i];
        bool person = sh.classes & 0x100, car = sh.classes & 0x200;
        // (a person's narrow sphere about his waist, a car's whole hull: physx_crates found these the ones)
        if (!(person && sh.kind == MML_SHAPE_SPHERE && (sh.classes & 0x1)) && !(car && sh.kind == MML_SHAPE_BOX && (sh.classes & 0x2)))
            continue;
        fsm::Shape &m = f.shape[f.shapes++];
        m = fsm::Shape{};
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

// The frame's record for the 64-bit half, once a frame: right after the mission's tick while it runs (the cars and
// people where this frame draws them: written at FRAME, before the tick, they were a tick behind, 0.13 m on average
// and 0.9 m at most for the cars measured), at FRAME while it does not (a menu, the pause: no step).
void Record(float dt, bool run) {
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
    f.meshes = g_meshCollision;
    f.shapes = 0;
    if (f.sources && !g_meshCollision) Shapes(lo, hi, dt);
    H->channel_write(g_frameCh, &f, uint32_t(offsetof(fsm::Frame, shape) + f.shapes * sizeof(fsm::Shape)));
    if (f.sources && run && g_meshCollision) {
        int64_t t0 = Qpc();
        Scenery(lo, hi);
        Movers(lo, hi, dt);
        double ms = Ms(t0, Qpc());
        S.meshSum += ms, S.meshMax = std::max(S.meshMax, ms), S.meshFrames++;
    }
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
    if (!g_meshCollision && g_rec.shapes) MeasureLag();
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
    case MML_EVENT_FRAME:
        g_frame++;
        if (H->mission_state() != MML_MISSION_RUNNING) Record(ev->dt, false);
        break;
    case MML_EVENT_MISSION_TICK: Record(ev->dt, true); break;
    case MML_EVENT_MISSION_CLOSE:   // (the world goes: the sources and the gas with it)
        g_sources.clear();
        g_clear++;
        g_haveOrigin = false;
        g_bonesLast.clear();
        g_last.clear();
        g_haveFocus = false;
        g_sceneSig = 0;
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
              {"gpu_ms", g_done.gpuMs}, {"sources", sources},
              {"collision", g_meshCollision ? "meshes" : "shapes"}, {"shapes", g_rec.shapes},
              {"shape_lag_m", {{"mean", g_lag.n ? g_lag.sum / g_lag.n : 0.0}, {"max", g_lag.max}, {"cars_frames", g_lag.n}}},
              {"movers", {{"count", g_moverRec.count}, {"vertices", g_moverRec.vertices}, {"triangles", g_moverRec.triangles}}},
              {"scenery", {{"serial", g_sceneSerial}, {"vertices", g_sceneVertices}, {"triangles", g_sceneTriangles}, {"in_flow", g_done.sceneTriangles}}},
              {"meshes_ms", {{"mean", S.meshFrames ? S.meshSum / S.meshFrames : 0.0}, {"max", S.meshMax}}},
              {"origin", g_haveOrigin ? json{g_origin[0], g_origin[1], g_origin[2]} : json()}, {"ground", g_ground},
              {"wind", {g_wind[0], g_wind[1], g_wind[2]}}, {"error", err}});
    return 0;
}
// Which way the game's faces point, by (v2 - v1) x (v3 - v1) (Flow's normal: what is behind it is inside): of the
// scenery's near-horizontal faces below the sources (the ground), how many point up; of a car's, how many point away
// from its middle. The two signs Flow's inside depends on, measured, not assumed.
int CmdWinding(const char *, MmlReply *r, void *) {
    auto *hdr = reinterpret_cast<const fsm::Scenery *>(g_sceneBuf.data());
    const float *pos = reinterpret_cast<const float *>(hdr + 1);
    const uint32_t *idx = reinterpret_cast<const uint32_t *>(pos + 3 * hdr->vertices);
    auto normal = [](const float *a, const float *b, const float *c, float n[3]) {
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        n[0] = u[1] * v[2] - u[2] * v[1], n[1] = u[2] * v[0] - u[0] * v[2], n[2] = u[0] * v[1] - u[1] * v[0];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0) n[0] /= l, n[1] /= l, n[2] /= l;
        return l > 0;
    };
    uint32_t up = 0, down = 0;
    for (uint32_t t = 0; t < hdr->triangles && g_sceneSerial; t++) {
        float n[3];
        const float *a = pos + 3 * idx[3 * t], *b = pos + 3 * idx[3 * t + 1], *c = pos + 3 * idx[3 * t + 2];
        if (!normal(a, b, c, n) || std::fabs(n[1]) < 0.9f || (a[1] + b[1] + c[1]) / 3 > g_ground + 0.5f) continue;
        (n[1] > 0 ? up : down)++;
    }
    uint32_t out = 0, in = 0;
    const fsm::Movers &rec = g_moverRec;
    const float *mpos = reinterpret_cast<const float *>(g_moverBuf.data() + sizeof(fsm::Movers));
    const uint32_t *midx = reinterpret_cast<const uint32_t *>(mpos + 6 * rec.vertices);
    for (uint32_t i = 0; i < rec.count; i++) {
        const fsm::Mover &m = rec.mover[i];
        if (m.kind != fsm::kCar || !m.vertices) continue;
        float mid[3] = {};
        for (uint32_t v = 0; v < m.vertices; v++)
            for (int k = 0; k < 3; k++) mid[k] += mpos[3 * (m.firstVertex + v) + k] / m.vertices;
        for (uint32_t t = 0; t < m.triangles; t++) {
            const uint32_t *tri = midx + 3 * (m.firstTriangle + t);
            const float *a = mpos + 3 * (m.firstVertex + tri[0]), *b = mpos + 3 * (m.firstVertex + tri[1]), *c = mpos + 3 * (m.firstVertex + tri[2]);
            float n[3];
            if (!normal(a, b, c, n)) continue;
            float d = 0;
            for (int k = 0; k < 3; k++) d += n[k] * ((a[k] + b[k] + c[k]) / 3 - mid[k]);
            (d > 0 ? out : in)++;
        }
    }
    Reply(r, {{"ground_faces", {{"up", up}, {"down", down}}}, {"car_faces", {{"out", out}, {"in", in}}},
              {"note", "Flow takes what is behind (v2 - v1) x (v3 - v1) as inside: the ground's faces up and the car's out mean "
                       "the winding as Flow wants it (flip 0)"}});
    return 0;
}

int CmdCollision(const char *args, MmlReply *r, void *) {
    json a = Args(args);
    std::string mode = a.value("mode", std::string());
    if (mode == "shapes" || mode == "meshes") {
        g_meshCollision = mode == "meshes";
        g_sceneSig = 0;   // (the scenery written again when the meshes are back)
    } else if (!mode.empty()) {
        H->reply_error(r, "mode: shapes or meshes");
        return 1;
    }
    Reply(r, {{"mode", g_meshCollision ? "meshes" : "shapes"}});
    return 0;
}

int CmdReset(const char *, MmlReply *r, void *) {
    S = Stats{};
    g_lag = Lag{};
    Reply(r, {{"reset", true}});
    return 0;
}

// Where the crosshair looks (the ray through the screen's middle, to the first thing it meets, the player left out):
// a point just off that surface; with nothing in 80 m, 4 m in front of the player on the ground.
bool Aimed(double at[3], float radius) {
    float o[3], d[3];
    if (H->mission_state() != MML_MISSION_RUNNING) return false;
    if (H->camera_ray(M, 400, 300, o, d)) {
        const float to[3] = {o[0] + 80 * d[0], o[1] + 80 * d[1], o[2] + 80 * d[2]};
        MmlHit hit{};
        hit.size = sizeof hit;
        if (H->world_ray(M, o, to, MML_WORLD_ALL, H->human_player(M), &hit)) {
            // (off the surface by the source's radius, so that it is not inside what it stands on)
            for (int k = 0; k < 3; k++) at[k] = hit.pos[k] + hit.normal[k] * radius;
            return true;
        }
    }
    float dir[3];
    if (!Player(at, dir)) return false;
    for (int k = 0; k < 3; k += 2) at[k] += dir[k] * 4;
    double y;
    if (Ground(at, &y)) at[1] = y;
    at[1] += radius;
    return true;
}

// The newest fire (or `id`'s source) moved to `at`, no longer following a car; none: a fire made there.
std::string Move(const json &a, uint32_t *id) {
    uint32_t want = a.value("id", 0u);
    auto it = std::find_if(g_sources.rbegin(), g_sources.rend(),
                           [&](const Source &s) { return want ? s.id == want : s.kind == fsm::kFire; });
    if (want && it == g_sources.rend()) return "no such source";
    double at[3];
    float radius = it != g_sources.rend() ? it->radius : 0.5f;
    if (a.contains("at") && a["at"].is_array() && a["at"].size() == 3) {
        for (int k = 0; k < 3; k++) at[k] = a["at"][k].get<double>();
    } else if (!Aimed(at, radius)) {
        return "no mission running";
    }
    if (it == g_sources.rend()) return Emit({{"kind", "fire"}, {"at", {at[0], at[1], at[2]}}}, id);
    memcpy(it->at, at, sizeof at);
    it->car = nullptr;
    *id = it->id;
    // (far from the origin Flow's floats lose their fineness: a new origin, the old gas blown away)
    if (std::fabs(at[0] - g_origin[0]) > 500 || std::fabs(at[2] - g_origin[2]) > 500) SetOrigin(at);
    return "";
}

int CmdMove(const char *args, MmlReply *r, void *) {
    uint32_t id = 0;
    std::string err;
    try {
        err = Move(Args(args), &id);
    } catch (const json::exception &e) {
        err = e.what();
    }
    if (!err.empty()) { H->reply_error(r, err.c_str()); return 1; }
    auto s = std::find_if(g_sources.begin(), g_sources.end(), [id](const Source &x) { return x.id == id; });
    Reply(r, {{"id", id}, {"at", {s->at[0], s->at[1], s->at[2]}}});
    return 0;
}

void KeyFire(void *) {
    uint32_t id;
    std::string err = Emit({{"kind", "fire"}}, &id);
    if (!err.empty()) H->log(M, MML_LOG_WARN, "F10: %s", err.c_str());
}
void KeyMove(void *) {
    uint32_t id;
    std::string err = Move(json::object(), &id);
    if (!err.empty()) H->log(M, MML_LOG_WARN, "F5: %s", err.c_str());
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
    g_sceneCh = H->channel_open(mod, "scenery", fsm::kSceneryBytes);
    g_moverCh = H->channel_open(mod, "movers", fsm::kMoversBytes);
    if (!g_frameCh || !g_viewCh || !g_doneCh || !g_sceneCh || !g_moverCh) return 1;
    Kept k{};
    if (H->state_get(mod, "sources", &k, sizeof k) == sizeof k && k.count <= fsm::kMaxSources) {   // (a reload)
        g_sources.assign(k.source, k.source + k.count);
        g_nextId = k.nextId, g_haveOrigin = k.haveOrigin, g_ground = k.ground;
        memcpy(g_origin, k.origin, sizeof g_origin);
        memcpy(g_wind, k.wind, sizeof g_wind);
    }
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_MISSION_TICK, MML_EVENT_MISSION_CLOSE, MML_EVENT_ACTOR_DESTROYED})
        H->subscribe(mod, t, OnEvent, nullptr);
    // (early among the passes: its picture is the whole frame, so the passes after it are laid over the gas)
    if (H->gfx_pass(mod, 50, Pass, nullptr) <= 0) {
        H->log(mod, MML_LOG_ERROR, "the graphics bridge refused the pass: flow_smoke needs \"gfx\": true on at the game's start");
        return 1;
    }
    H->command(mod, "emit", "{kind: smoke|fire|steam, ahead | at | on: \"car\", radius, strength, velocity, seconds} -> {id}: a source of gas (NVIDIA Flow on the GPU)", CmdEmit, nullptr);
    H->command(mod, "move", "{id, at} -> the newest fire (or id's source) moved where the crosshair looks (or to at); none: a fire made there. F5", CmdMove, nullptr);
    H->command(mod, "stop", "{id} -> that source taken away (its gas fades)", CmdStop, nullptr);
    H->command(mod, "clear", "{} -> every source and all the gas taken away", CmdClear, nullptr);
    H->command(mod, "wind", "{velocity: [x, y, z]} -> the air's own motion, m/s", CmdWind, nullptr);
    H->command(mod, "wait", "{ms} -> how long the pass waits for this frame's picture (at most 100)", CmdWait, nullptr);
    H->command(mod, "state", "{} -> frames drawn, late, the round trip, Flow's blocks and times, the sources", CmdState, nullptr);
    H->command(mod, "collision", "{mode: shapes | meshes} -> what the gas meets: the game's shapes (a person's sphere, a car's box; the default) or the drawn meshes", CmdCollision, nullptr);
    H->command(mod, "winding", "{} -> which way the game's faces point by Flow's normal: the ground's up or down, a car's out or in", CmdWinding, nullptr);
    H->command(mod, "reset", "{} -> the measures started again", CmdReset, nullptr);
    H->hotkey(mod, "F5", KeyMove, nullptr);
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
