// physx_crates: a mod in two halves (docs/MODDING.md "64-bit halves"), PhysX 5's crates in the game. Its 64-bit half
// (physx_crates64.cpp, run by the sidecar) steps them with PhysX 5, which exists only for 64 bits; this 32-bit half
// gives it the game's world and shows the crates as props (models of the game's: bedna00, a wooden crate).
//
// `physx_crates.drop` makes a pile in front of the player: the crates' places, and the static world around them as the
// game's own collision keeps it (world_shapes: the triangles, boxes, spheres and poles of the crates' box and 8 m
// around), in the channel "setup". At each frame's start (FRAME) this half writes the channel "frame": the frame's number, its time, its dt,
// whether the world runs, and the moving objects around the pile (world_shapes again: people's spheres, cars' boxes),
// which push the crates. The 64-bit half steps that frame and writes the channel "poses"; this half reads it when the
// frame's scene is about to be drawn (RENDER_BEGIN: the mission's tick has come between) and places the props, and
// counts how many frames late the poses are: 0, this very frame's step.
//
// When a crate comes near the side of the region the statics cover (pushed, rolled away), the region is taken again
// around the crates (as they are, their velocities too). They live across a reload: their poses are kept (state_put) and given to the new
// 64-bit half as a new setup.
//   call physx_crates.drop {"count": 27, "ahead": 4, "height": 1, "model": "bedna00.i3d", "half": 0.324, "mass": 25}
//   call physx_crates.clear
//   call physx_crates.state -> {"bodies", "frames", "late": {"0", "1", "2+"}, "none", "round_trip_ms": {"mean",
//                               "max"}, "ask_to_done_ms", "step_ms", "awake", "lost", "movers", "triangles",
//                               "statics", "setups"...}: what this half saw
//   call physx_crates.reset -> the measures started again
//   call physx_crates.side  -> the 64-bit half's own count (physx_crates64.cpp)
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/mml.h"
#include "physx_crates.h"

using nlohmann::json;

namespace {
const MmlHost *H;
MmlMod *M;
MmlChannel *g_setupCh, *g_frameCh, *g_posesCh;
// The region the statics cover: the crates' box, kMargin more each way (kDown under the lowest, kUp over the highest),
// at most 2 * kMaxHalf wide; taken again when a crate comes within kNear of its side.
const float kMargin = 8, kNear = 3, kMaxHalf = 32, kDown = 6, kUp = 12;

std::unique_ptr<crates::Setup> g_setup = std::make_unique<crates::Setup>();
crates::Frame g_frameRec;
crates::Poses g_poses;
std::vector<MmlShape> g_shapes;

// the pile
std::string g_model = "bedna00.i3d";
float g_half = 0.324f, g_mass = 25;   // (bedna00.4ds: a cube of 0.648 m about its origin)
uint32_t g_count, g_serial;
void *g_props[crates::kMaxBodies];
bool g_shown[crates::kMaxBodies];
float g_center[3];             // the region's centre
uint64_t g_frame;              // FRAME events
uint64_t g_lastSetupFrame;
bool g_restore;
uint32_t g_statics, g_truncated, g_setups;
double g_freqMs;
struct Stats {
    uint64_t frames, late0, late1, late2, none;
    double rtSum, rtMax, workSum, stepSum, stepMax;
} S;

uint64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
double Ms(int64_t from, int64_t to) { return (to - from) / g_freqMs; }

void Forget() {   // (the props are gone with the mission, or about to go with the unload)
    for (void *&p : g_props) p = nullptr;
    g_count = 0;
}
void RemoveProps() {
    for (uint32_t i = 0; i < crates::kMaxBodies; i++)
        if (g_props[i]) H->prop_remove(M, g_props[i]);
    Forget();
}

// The region around the pile's bodies (g_setup->body, g_props), the lost ones left out: the crates outside it (lost,
// or flung farther than the widest region reaches) are out of the pile for good, their props removed; then the static
// world of the region, as the game's collision keeps it, into the setup.
void Region(const uint8_t *lost) {
    crates::Setup &s = *g_setup;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, sum[3] = {0, 0, 0};
    uint32_t in = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        if (lost && lost[i]) continue;
        const float *p = s.body[i].p;
        for (int k = 0; k < 3; k++) lo[k] = (std::min)(lo[k], p[k]), hi[k] = (std::max)(hi[k], p[k]), sum[k] += p[k];
        in++;
    }
    if (!in) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = sum[0] = sum[1] = sum[2] = 0, in = 1;   // (none left: an empty pile)
    for (int k : {0, 2}) {   // (wider than allowed: about their centre)
        lo[k] -= kMargin, hi[k] += kMargin;
        float c = sum[k] / in;
        if (hi[k] - lo[k] > 2 * kMaxHalf) lo[k] = c - kMaxHalf, hi[k] = c + kMaxHalf;
    }
    lo[1] -= kDown, hi[1] += kUp;
    memcpy(s.lo, lo, sizeof lo);
    memcpy(s.hi, hi, sizeof hi);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        const float *p = s.body[i].p;
        // (kNear inside its sides: a crate the widest region leaves that near a side would take it again at once)
        bool out = (lost && lost[i]) || p[0] < lo[0] + kNear || p[0] > hi[0] - kNear || p[2] < lo[2] + kNear || p[2] > hi[2] - kNear;
        if (out) {
            if (g_props[i]) H->prop_remove(M, g_props[i]);
            continue;
        }
        s.body[kept] = s.body[i], g_props[kept] = g_props[i], g_shown[kept] = g_shown[i];
        kept++;
    }
    for (uint32_t i = kept; i < g_count; i++) g_props[i] = nullptr;
    g_count = kept;
    g_shapes.resize(crates::kMaxStatics);
    int n = H->world_shapes(M, s.lo, s.hi, MML_WORLD_STATIC, g_shapes.data(), crates::kMaxStatics);
    g_truncated = n > static_cast<int>(crates::kMaxStatics) ? n - crates::kMaxStatics : 0;
    if (n > static_cast<int>(crates::kMaxStatics)) n = crates::kMaxStatics;
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        const MmlShape &sh = g_shapes[i];
        crates::Static &st = s.shape[k];
        st = crates::Static{};
        switch (sh.kind) {
        case MML_SHAPE_TRIANGLE:
            st.kind = crates::kTriangle;
            memcpy(st.v, sh.corner, 9 * sizeof(float));
            memcpy(st.v + 9, sh.normal, 3 * sizeof(float));
            break;
        case MML_SHAPE_BOX:
            st.kind = crates::kBox;
            memcpy(st.v, sh.center, 3 * sizeof(float));
            memcpy(st.v + 3, sh.half, 3 * sizeof(float));
            memcpy(st.v + 6, sh.axes, 9 * sizeof(float));
            break;
        case MML_SHAPE_SPHERE:
            st.kind = crates::kSphere;
            memcpy(st.v, sh.center, 3 * sizeof(float));
            st.v[3] = sh.half[0];
            break;
        case MML_SHAPE_POLE:
            st.kind = crates::kPole;
            st.v[0] = sh.center[0], st.v[1] = sh.center[2], st.v[3] = sh.half[0];
            break;
        default: continue;
        }
        k++;
    }
    s.statics = g_statics = k;
    for (int i = 0; i < 3; i++) g_center[i] = (s.lo[i] + s.hi[i]) * 0.5f;
}

// A new setup for the bodies in g_setup->body (a new serial: the 64-bit half builds a new scene).
void SendSetup() {
    crates::Setup &s = *g_setup;
    g_serial = static_cast<uint32_t>(Qpc()) | 1u;   // (another load's serial, never a stale poses' one)
    s.serial = g_serial;
    s.bodies = g_count;
    s.half = g_half;
    s.mass = g_mass;
    H->channel_write(g_setupCh, &s, static_cast<uint32_t>(offsetof(crates::Setup, shape) + s.statics * sizeof(crates::Static)));
    g_lastSetupFrame = g_frame;
    g_setups++;
}

bool Ground(const float at[3], float *y) {
    const float from[3] = {at[0], at[1] + 3, at[2]}, to[3] = {at[0], at[1] - 10, at[2]};
    MmlHit hit{};
    hit.size = sizeof hit;
    if (!H->world_ray(M, from, to, MML_WORLD_STATIC, nullptr, &hit)) return false;
    *y = hit.pos[1];
    return true;
}

bool MakeProps() {
    for (uint32_t i = 0; i < g_count; i++) {
        if (!g_props[i]) g_props[i] = H->prop_create(M, g_model.c_str(), nullptr, g_setup->body[i].p, nullptr, 1);
        if (!g_props[i]) return false;
        g_shown[i] = true;
    }
    return true;
}

std::string Drop(const json &a) {
    if (H->mission_state() != MML_MISSION_RUNNING) return "no mission running";
    void *player = H->human_player(M);
    float w[16];
    if (!player || !H->frame_world(M, H->actor_frame(M, player), w)) return "no player";
    uint32_t count = a.value("count", 27u);
    if (count < 1 || count > crates::kMaxBodies) return "count: 1.." + std::to_string(crates::kMaxBodies);
    float ahead = a.value("ahead", 4.0f), height = a.value("height", 1.0f);
    std::string model = a.value("model", std::string("bedna00.i3d"));
    float half = a.value("half", model == "bedna00.i3d" ? 0.324f : 0.3f), mass = a.value("mass", 25.0f);
    if (half < 0.05f || half > 2 || mass < 0.1f) return "half: 0.05..2 m, mass: 0.1 kg and more";
    RemoveProps();
    g_model = model, g_half = half, g_mass = mass;
    // in front of him (his model's z, flat), on the ground there
    float fx = w[8], fz = w[10], fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 0.01f) fx = 0, fz = 1, fl = 1;
    float c[3] = {w[12] + fx / fl * ahead, w[13], w[14] + fz / fl * ahead};
    float groundY = w[13];
    Ground(c, &groundY);
    c[1] = groundY;
    // a stack of layers of 3 x 3, a little apart and turned, falling from `height`
    float step = 2 * half + 0.04f;
    uint32_t seed = static_cast<uint32_t>(Qpc());
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f; };
    for (uint32_t i = 0; i < count; i++) {
        uint32_t layer = i / 9, cell = i % 9;
        float x = (static_cast<float>(cell % 3) - 1) * step + rnd() * 0.1f, z = (static_cast<float>(cell / 3) - 1) * step + rnd() * 0.1f;
        float yaw = rnd() * 0.6f;
        crates::Body &b = g_setup->body[i];
        b = crates::Body{{c[0] + x, groundY + height + half + layer * step, c[2] + z}, {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}, {0, 0, 0}, {0, 0, 0}};
    }
    g_count = count;
    if (!MakeProps()) {
        RemoveProps();
        return "the model " + model + " was not made (prop_create)";
    }
    Region(nullptr);
    SendSetup();
    return "";
}

// The crates where the poses say, the region taken again when they have left it.
void Place() {
    uint64_t v = 0;
    uint32_t got = H->channel_read(g_posesCh, &g_poses, sizeof g_poses, &v);
    S.frames++;
    if (got < offsetof(crates::Poses, body) || g_poses.serial != g_serial || g_poses.bodies != g_count) { S.none++; return; }
    uint64_t late = g_frame - g_poses.frame;
    (late == 0 ? S.late0 : late == 1 ? S.late1 : S.late2)++;
    double rt = Ms(g_poses.askQpc, static_cast<int64_t>(Qpc()));
    S.rtSum += rt;
    if (rt > S.rtMax) S.rtMax = rt;
    S.workSum += Ms(g_poses.askQpc, g_poses.doneQpc);
    S.stepSum += g_poses.stepMs;
    if (g_poses.stepMs > S.stepMax) S.stepMax = g_poses.stepMs;
    uint8_t lost[crates::kMaxBodies];
    bool edge = false;
    const crates::Setup &s = *g_setup;
    for (uint32_t i = 0; i < g_count; i++) {
        const crates::Body &b = g_poses.body[i];
        lost[i] = b.p[1] < s.lo[1] - 20;
        if (lost[i] != !g_shown[i] && g_props[i]) H->prop_show(M, g_props[i], !lost[i]), g_shown[i] = !lost[i];
        if (lost[i] || !g_props[i]) continue;
        const float rot[4] = {b.q[3], -b.q[0], -b.q[1], -b.q[2]};   // (the game's rotation: PhysX's conjugate)
        H->prop_place(M, g_props[i], b.p, rot, 0);
        // where it will be in half a second, near a side (not the bottom: a crate that falls through it has fallen
        // through the world, and is lost)
        float x = b.p[0] + b.v[0] * 0.5f, y = b.p[1] + b.v[1] * 0.5f, z = b.p[2] + b.v[2] * 0.5f;
        edge = edge || x < s.lo[0] + kNear || x > s.hi[0] - kNear || z < s.lo[2] + kNear || z > s.hi[2] - kNear || y > s.hi[1] - kNear;
    }
    // a crate near the region's side: the statics taken again around the crates, which go on as they are (their
    // velocities too; a quarter of a second apart at least: a new scene costs about a millisecond in the 64-bit half)
    if (edge && g_frame - g_lastSetupFrame > 15) {
        memcpy(g_setup->body, g_poses.body, g_count * sizeof(crates::Body));
        Region(lost);
        SendSetup();
    }
}

void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_FRAME: {
        g_frame++;
        if (g_restore && H->mission_state() == MML_MISSION_RUNNING) {   // a reload's crates, made again where they were
            g_restore = false;
            if (MakeProps()) {
                Region(nullptr);
                SendSetup();
            } else {
                RemoveProps();
            }
        }
        if (!g_count || g_restore) break;
        crates::Frame &f = g_frameRec;
        f.frame = g_frame;
        f.qpc = static_cast<int64_t>(Qpc());
        f.dt = ev->dt;
        f.run = H->mission_state() == MML_MISSION_RUNNING;   // (not MML_MISSION_PAUSED: a menu stopped the world)
        f.serial = g_serial;
        f.movers = 0;
        int n = H->world_shapes(M, g_setup->lo, g_setup->hi, MML_WORLD_MOVING, g_shapes.data(), crates::kMaxMovers);
        if (n > static_cast<int>(crates::kMaxMovers)) n = crates::kMaxMovers;
        // The game keeps two shapes for a person and two for a car (both builds): a person's sphere of 0.3 m about
        // his waist (classes 0x101) and a wide one of 1.2 m (0x102); a car's body over its wheels (0x201) and its
        // whole hull down to the ground (0x202). The crates meet a person as the narrow sphere drawn out into an
        // upright capsule from his feet to his head (the sphere alone passes over a crate on the ground), and a car
        // as its whole hull.
        for (int i = 0; i < n; i++) {
            const MmlShape &sh = g_shapes[i];
            if (sh.kind != MML_SHAPE_BOX && sh.kind != MML_SHAPE_SPHERE) continue;
            bool person = sh.classes & 0x100, car = sh.classes & 0x200;
            if ((person && !(sh.classes & 0x1)) || (car && !(sh.classes & 0x2))) continue;
            crates::Mover &m = f.mover[f.movers++];
            m = crates::Mover{};
            m.id = reinterpret_cast<uintptr_t>(sh.prim);
            m.kind = sh.kind == MML_SHAPE_BOX ? crates::kBox : crates::kSphere;
            memcpy(m.center, sh.center, sizeof m.center);
            memcpy(m.half, sh.half, sizeof m.half);
            memcpy(m.axes, sh.axes, sizeof m.axes);
            if (person && m.kind == crates::kSphere) {   // (the sphere is 0.8 m over his feet: from 0.05 m to 1.8 m)
                float feet = sh.center[1] - 0.8f, r = sh.half[0];
                m.kind = crates::kCapsule;
                m.center[1] = feet + (0.05f + 1.8f) * 0.5f;
                m.half[1] = (1.8f - 0.05f) * 0.5f - r;
            }
        }
        H->channel_write(g_frameCh, &f, static_cast<uint32_t>(offsetof(crates::Frame, mover) + f.movers * sizeof(crates::Mover)));
        break;
    }
    case MML_EVENT_RENDER_BEGIN:
        if (g_count && !g_restore && ev->world) Place();
        break;
    case MML_EVENT_MISSION_CLOSE:   // (the mission takes every prop with it)
        Forget();
        g_restore = false;
        break;
    default: break;
    }
}

void Reply(MmlReply *r, const json &j) {
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    H->reply_append(r, s.c_str(), s.size());
}

int CmdDrop(const char *args, MmlReply *r, void *) {
    json a = json::parse(args ? args : "{}", nullptr, false);
    if (!a.is_object()) a = json::object();
    std::string err = Drop(a);
    if (!err.empty()) { H->reply_error(r, err.c_str()); return 1; }
    Reply(r, {{"bodies", g_count}, {"model", g_model}, {"half", g_half}, {"mass", g_mass}, {"statics", g_statics},
              {"statics_left_out", g_truncated}, {"center", {g_center[0], g_center[1], g_center[2]}}});
    return 0;
}
int CmdClear(const char *, MmlReply *r, void *) {
    RemoveProps();
    SendSetup();   // (no bodies: the 64-bit half's scene goes)
    Reply(r, {{"cleared", true}});
    return 0;
}
int CmdState(const char *, MmlReply *r, void *) {
    uint64_t seen = S.late0 + S.late1 + S.late2;
    Reply(r, {{"bodies", g_count}, {"frames", S.frames},
              {"late", {{"0", S.late0}, {"1", S.late1}, {"2+", S.late2}}}, {"none", S.none},
              {"round_trip_ms", {{"mean", seen ? S.rtSum / seen : 0.0}, {"max", S.rtMax}}},
              {"ask_to_done_ms", seen ? S.workSum / seen : 0.0},
              {"step_ms", {{"mean", seen ? S.stepSum / seen : 0.0}, {"max", S.stepMax}, {"last", g_poses.stepMs}}},
              {"substeps", g_poses.substeps}, {"awake", g_poses.awake}, {"lost", g_poses.lost}, {"movers", g_poses.movers},
              {"triangles", g_poses.triangles}, {"statics", g_statics}, {"statics_left_out", g_truncated}, {"setups", g_setups},
              {"center", {g_center[0], g_center[1], g_center[2]}}, {"running", H->mission_state() == MML_MISSION_RUNNING}});
    return 0;
}
int CmdReset(const char *, MmlReply *r, void *) {
    S = Stats{};
    Reply(r, {{"reset", true}});
    return 0;
}

// what survives a reload: the pile (its model, size, mass and the bodies as the last poses had them)
struct Kept {
    char model[64];
    float half, mass;
    uint32_t count;
    crates::Body body[crates::kMaxBodies];
};
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, channel_read) || !MML_HAS(H, world_shapes) || !MML_HAS(H, prop_create)) return 1;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_setupCh = H->channel_open(mod, "setup", sizeof(crates::Setup));
    g_frameCh = H->channel_open(mod, "frame", sizeof(crates::Frame));
    g_posesCh = H->channel_open(mod, "poses", sizeof(crates::Poses));
    if (!g_setupCh || !g_frameCh || !g_posesCh) return 1;
    g_shapes.resize(crates::kMaxStatics);
    Kept k{};
    if (H->state_get(mod, "pile", &k, sizeof k) == sizeof k && k.count && k.count <= crates::kMaxBodies) {
        k.model[sizeof k.model - 1] = 0;
        g_model = k.model, g_half = k.half, g_mass = k.mass, g_count = k.count;
        memcpy(g_setup->body, k.body, k.count * sizeof(crates::Body));
        g_restore = true;   // (at the next frame, on the game thread)
    }
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_RENDER_BEGIN, MML_EVENT_MISSION_CLOSE})
        H->subscribe(mod, t, OnEvent, nullptr);
    H->command(mod, "drop", "{count, ahead, height, model, half, mass} -> a pile of crates in front of the player, stepped by PhysX 5 in the 64-bit half", CmdDrop, nullptr);
    H->command(mod, "clear", "{} -> the crates taken away", CmdClear, nullptr);
    H->command(mod, "state", "{} -> what this half saw: frames, how late the poses were, the round trip, the step", CmdState, nullptr);
    H->command(mod, "reset", "{} -> the measures started again", CmdReset, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *mod) {
    Kept k{};   // (count 0: no pile)
    snprintf(k.model, sizeof k.model, "%s", g_model.c_str());
    k.half = g_half, k.mass = g_mass;
    // the last poses of this pile but the lost crates, or (unloaded before its crates were made again) the bodies as
    // they came
    bool fresh = !g_restore && g_poses.serial == g_serial && g_poses.bodies == g_count;
    for (uint32_t i = 0; i < g_count; i++) {
        const crates::Body &b = fresh ? g_poses.body[i] : g_setup->body[i];
        if (!fresh || b.p[1] >= g_setup->lo[1] - 20) k.body[k.count++] = b;
    }
    H->state_put(mod, "pile", &k, sizeof k);
    Forget();   // (the loader takes the mod's props with its unload)
}
