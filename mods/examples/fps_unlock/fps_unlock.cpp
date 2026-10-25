// fps_unlock: the frame rate freed, the game's time kept real (MafiaD3D11's A132, as a mod of the loader).
//
// How the game paces itself: its main loop asks LS3D's frame limiter (IGraph::GetTimer, `frameLimit` in
// game_build.h) for a frame of at least 16 ms and at most 83 ms. A shorter frame is slept to 16 ms, so the game never
// runs above 62.5 fps; the limiter's answer is the time every system of the game is given (the mission's tick, the
// recordings, the caches, the map, the cutscenes). Present also yields (Sleep(0)) before each frame.
// Two places changed, nothing else: the limiter's `jae` becomes `jmp` (it never waits and returns the frame's real
// time, in whole milliseconds; a long frame is still cut to 83 ms), and Present's `push 0; call Sleep` is jumped
// over. The game's time stays real: at 200 fps every system gets 5 ms a frame instead of 16 ms every 16 ms.
// What the game did once a frame instead of by the time breaks then; three are fixed here (the main menu's camera,
// CamTick and 1.0's Cam10; a person's step at a curb, Swept; the small rotations LS3D drops, which left a car
// creeping and its steering with a dead zone, RotationMatrix) and the others found go to knowledge/notes/game.md,
// "The frame rate".
//
// What it shows a mod author:
//   patching the game's code   Find, Apply, Restore   the build's addresses from mml/game/game_build.h; the bytes
//                              checked before writing (another tool may have changed them: then nothing is done);
//                              written on the game thread (mods load and get their events there, so no thread runs
//                              that code meanwhile); put back at the unload (a hot reload or the mod switched off
//                              leave the game as it was)
//   fixing a game function     CamTick       a hook (host->hook: chained, taken off with the mod) that calls the
//                              game's own function and corrects what it did once a frame
//   lengths made once a call   Swept, Walk   the game's own function given, for each call, the lengths of its
//   made lengths of the time                 step's time: its operands pointed at the mod's floats (in the heap)
//   an engine function fixed   RotationMatrix   LS3DF.dll's export hooked by its name, the fix checked against the
//                                            game's own function at load, everything else handed to it unchanged
//   pacing frames precisely    Pace         a frame-rate cap of its own, on a high-resolution waitable timer (the
//                              game's Sleep has a 1 ms grain at best)
//   measuring the frame        Measure       frames a second, the frame's time, the game's own clock step, and what
//                                            reached the display (gfx_show_stats: frame generation's pictures)
//   a setting switched live    OnKey, ReadSettings   the hotkey flips "enabled" with config_set; the change comes back
//                              as MML_EVENT_CONFIG_CHANGED like an edit of the file or the bridge's config.set
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
const MmlHost *H;
MmlMod *M;

// ---- settings
struct Settings {
    bool enabled = true;     // the two places patched
    int maxFps = 0;          // 0: no cap of the mod's own
    bool show = true;        // the line at the top left
    bool fixCamera = true;   // the main menu's camera stepped by the time (menuCamTick)
    bool fixWalk = true;     // a person's sweep made with lengths of its step's time (humanSweep): curbs as at 62.5 fps
    bool fixRotation = true; // LS3D's quaternion-to-matrix keeps rotations under 0.00049 rad (RotationMatrix)
    std::string hotkey = "F11";
    std::string showHotkey = "Shift+F11";   // switches "show"
} S;

// ---- the two places
struct Site {
    const char *name;
    uintptr_t at = 0;
    uint8_t original[2], patched[2];   // (the first two bytes are all that changes at either place)
};
Site g_sites[2] = {{"the limiter's minimum", 0, {0x73, 0x18}, {0xEB, 0x18}},     // jae +0x18 -> jmp +0x18
                   {"Present's Sleep(0)", 0, {0x6A, 0x00}, {0xEB, 0x02}}};       // push 0 -> jmp +2 (over the call)
bool g_known;                // the build's places found and as expected
enum class Owner { none, us, other } g_owner = Owner::none;
const char *g_why = "";      // why nothing was done
uintptr_t g_clock;           // LS3D's millisecond clock (the limiter reads it)

enum class State { original, patched, other };
State StateOf(const Site &s) {
    uint8_t b[2];
    if (!H->mem_read(s.at, b, 2)) return State::other;
    if (!memcmp(b, s.original, 2)) return State::original;
    if (!memcmp(b, s.patched, 2)) return State::patched;
    return State::other;
}

bool Find() {
    uintptr_t ls3d = H->module_base("LS3DF.dll");
    if (!ls3d || !game::Detect(ls3d)) { g_why = "not a build of the game whose addresses are known"; return false; }
    const game::Ls3dBuild &l = game::Ls3d();
    g_sites[0].at = ls3d + l.limiterMinJump;
    g_sites[1].at = ls3d + l.presentYield;
    // the clock: `mov eax, [clock]` 13 bytes before the jump (tools/check_builds.py checks the shape)
    uint8_t mov = 0;
    if (!H->mem_read(g_sites[0].at - 0x0D, &mov, 1) || mov != 0xA1 || !H->mem_read(g_sites[0].at - 0x0C, &g_clock, 4)) g_clock = 0;
    return true;
}

bool WriteAll(bool patch) {
    for (Site &s : g_sites)
        if (!H->mem_write(s.at, patch ? s.patched : s.original, 2)) return false;
    return true;
}

void Apply() {
    if (!g_known || g_owner != Owner::none) return;
    State a = StateOf(g_sites[0]), b = StateOf(g_sites[1]);
    if (a == State::patched && b == State::patched) {   // MafiaVR's ASI, an earlier copy of this mod that was kept...
        g_owner = Owner::other;
        H->log(M, MML_LOG_INFO, "already unlocked by another tool: left to it");
        return;
    }
    if (a != State::original || b != State::original) {
        g_why = "the two places hold bytes of neither the game nor the unlock (another tool changed them)";
        H->log(M, MML_LOG_WARN, "%s: nothing done", g_why);
        return;
    }
    if (!WriteAll(true)) {
        WriteAll(false);
        g_why = "the write failed";
        H->log(M, MML_LOG_ERROR, "%s: put back", g_why);
        return;
    }
    g_owner = Owner::us;
    H->log(M, MML_LOG_INFO, "unlocked: the limiter returns the frame's real time (83 ms the most), Present does not yield");
}

void Restore() {
    if (g_owner == Owner::us && StateOf(g_sites[0]) == State::patched && StateOf(g_sites[1]) == State::patched) {
        WriteAll(false);
        H->log(M, MML_LOG_INFO, "the game's own pacing back (62.5 fps the most)");
    }
    if (g_owner == Owner::us) g_owner = Owner::none;
}

// ---- the main menu's camera: a spring stepped once a call, made to step by the time
// GM_Menu's TickCamera (game_build.h menuCamTick, menuCam) moves the camera's eye and look point by their velocities
// times the frame's time, then springs the velocities toward their targets once a call: v = 0.98 v + 0.02 (target -
// point). At 62.5 fps the velocity builds up over about 0.8 s; at 800 fps over 60 ms, and frames of 0 ms spring it
// too: the camera leaps from one place to the next. The fix keeps the game's tick and does the spring again by the
// time: k = 1 - 0.98^(ms / 16), the same 0.02 at 16 ms, the same motion in seconds at any rate.
LARGE_INTEGER g_freq;   // (QueryPerformanceFrequency)
using CamTickFn = int(__fastcall *)(unsigned);
CamTickFn g_camOriginal;
bool g_camHooked;
int __fastcall CamTick(unsigned ms) {
    float *cam = reinterpret_cast<float *>(game::Exe().menuCam);   // (game thread: the game's own globals, read in place)
    float v0[6];
    memcpy(v0, cam, sizeof v0);
    int r = g_camOriginal(ms);
    if (S.enabled && S.fixCamera) {
        const float k = 1.0f - powf(0.98f, ms / 16.0f), keep = 1.0f - k;
        for (int i = 0; i < 3; i++) {
            cam[i] = v0[i] * keep + (cam[6 + i] - cam[12 + i]) * k;            // the look point's: its target - it
            cam[3 + i] = v0[3 + i] * keep + (cam[9 + i] - cam[15 + i]) * k;    // the eye's
        }
    }
    return r;
}

// The camera's path, for measuring (fps_unlock.camera): the scene's camera frame (`GameCamera`, the mission's scene
// + 0x17C: 1.0's menu loop moves it there, 1.2's TickCamera too) sampled every 20 ms at most, in both builds alike.
struct CamSample { float t, pos[3]; };
CamSample g_trace[600];
int g_traceN;
LARGE_INTEGER g_traceStart, g_traceLast;

void TraceCamera() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_traceStart.QuadPart || g_traceN >= static_cast<int>(sizeof g_trace / sizeof *g_trace) ||
        now.QuadPart - g_traceLast.QuadPart < g_freq.QuadPart / 50 || !game::g_exe)
        return;
    uintptr_t mission = 0, scene = 0, camera = 0;
    float pos[3];
    if (!H->mem_read(game::Exe().missionPtr, &mission, 4) || !H->mem_read(mission + game::kMissionScene, &scene, 4) ||
        !H->mem_read(scene + 0x17C, &camera, 4) || !H->mem_read(camera + game::kFrameWorld + 0x30, pos, sizeof pos))
        return;
    g_trace[g_traceN++] = {static_cast<float>((now.QuadPart - g_traceStart.QuadPart) / static_cast<double>(g_freq.QuadPart)),
                           {pos[0], pos[1], pos[2]}};
    g_traceLast = now;
}

// ---- 1.0's main menu camera: substeps counted in frames, made to count the time
// 1.0 has no TickCamera to hook: the camera is inlined into its menu loop, its state the loop's own locals
// (game_build.h menuCamLoop). Each frame, 10 substeps move the look point and the eye toward a middle point each
// (* 0.001) and the middle points toward their targets (* 0.002), with no time at all. Those constants are shared with
// the rest of the game (0.001 is used 218 times), so they are left alone: the loop's 12 `fmul dword [const]` operands
// are pointed at two floats of the mod's instead, set every frame from the game's step: a' = 1 - (1 - a)^(ms / 16),
// the game's own at 16 ms, the same motion in seconds at any rate. The operands are put back at the unload. The two
// floats live in the process's heap, not in this DLL: a mod that faults is unloaded without its mml_mod_unload, and
// the game's code would then read freed memory; the block is handed on to the next load (state_put) and reused.
float *g_k10;
struct Operand { uintptr_t at; uint32_t original; int slot; };
Operand g_ops10[12];
int g_ops10N;
bool g_cam10;            // the operands point at g_k10
unsigned g_step = 16;    // the game's last step, ms (Measure)

bool FindCam10() {
    // an earlier load's list (kept at once: a load that faulted could not put it back): each operand must still be
    // the game's constant or this block
    size_t n = H->state_get(M, "ops10", g_ops10, sizeof g_ops10);
    if (n == sizeof g_ops10) {
        g_ops10N = 12;
        for (const Operand &o : g_ops10) {
            uint32_t now = 0;
            H->mem_read(o.at, &now, 4);
            if (now != o.original && now != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_k10[o.slot]))) g_ops10N = 0;
            else if (now != o.original) g_cam10 = true;   // (left pointing at the block by a load that faulted)
        }
        if (g_ops10N) return true;
    }
    const uintptr_t lo = game::Exe().menuCamLoop, hi = game::Exe().menuCamLoopEnd;
    uint8_t code[0x300];
    if (!lo || hi <= lo || hi - lo > sizeof code || !H->mem_read(lo, code, hi - lo)) return false;
    int count[2] = {0, 0};
    g_ops10N = 0;
    for (uintptr_t i = 0; i + 6 <= hi - lo && g_ops10N < 12; i++) {
        if (code[i] != 0xD8 || code[i + 1] != 0x0D) continue;   // fmul dword ptr [abs32]
        uint32_t c;
        float v;
        memcpy(&c, code + i + 2, 4);
        if (!H->mem_read(c, &v, 4)) continue;
        int slot = fabsf(v - 0.001f) < 1e-7f ? 0 : fabsf(v - 0.002f) < 1e-7f ? 1 : -1;
        if (slot < 0) continue;
        g_ops10[g_ops10N++] = {lo + i + 2, c, slot};
        count[slot]++;
    }
    if (g_ops10N != 12 || count[0] != 6 || count[1] != 6) return false;   // (tools/check_builds.py checks the same)
    H->state_put(M, "ops10", g_ops10, sizeof g_ops10);
    return true;
}

void Cam10(bool on) {
    if (on == g_cam10 || !g_ops10N) return;
    for (int i = 0; i < g_ops10N; i++) {
        uint32_t want = on ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_k10[g_ops10[i].slot])) : g_ops10[i].original;
        H->mem_write(g_ops10[i].at, &want, 4);   // (game thread: the menu loop is not running this code now)
    }
    g_cam10 = on;
}

void StepCam10() {
    if (!g_cam10) return;
    const float ms = static_cast<float>(g_step < 249 ? g_step : 249);   // (the menu loop's own maximum)
    g_k10[0] = 1.0f - powf(1.0f - 0.001f, ms / 16.0f);
    g_k10[1] = 1.0f - powf(1.0f - 0.002f, ms / 16.0f);
}

// ---- a person's step: the sweep's lengths made once a call, made lengths of the time
// A person on foot moves by sweeping three spheres along the frame's move (game_build.h humanSweep; humanSweepOwner
// for one standing in a vehicle) and sliding them along whatever they touch (humanSlide, humanSlideOwner: up to 10
// slides, each one's rest projected on the contact's plane). Three lengths in it are made once a call, tuned for the
// game's 16 ms step (knowledge/notes/game.md, "A person's step"):
//   start   the sphere starts 0.005 m higher than the result's reference (0.405 above the feet, the result its centre
//           minus 0.4); in the vehicle sweep 0 (0.4 and 0.4);
//   aim     the move is aimed 0.015 m lower;
//   skin    every slide starts again 0.01 m off the contact, along its normal; that also makes a person stand 0.01 m
//           above the ground.
// (and, outside the sweep, the fall on the ground: restarted every frame, a frame's move goes down 9.81 t^2: AimOf).
// At 62.5 fps they are 0.31, 0.94 and 0.63 m/s; at 400 fps six times as much. A curb's edge is climbed when the slide
// along it goes up, which the forward step decides against those lengths: in 2.5 ms steps it goes down and a walking
// person stops at every curb. Made rates of the time, the three give the same motion at every step (measured with
// fps_unlock.sweepprobe: 1, 2.5 and 4 ms steps agree within 1 cm and the 16 ms step within 3 cm at the owner's curb;
// curbs, stairs and the freeride spawn's curb of growing height climbed exactly where 62.5 fps climbs them; scaling
// the aim alone left the other two lifting a person up walls). So each call's lengths are k = ms / 16 of the
// game's (k = 1 at 16 ms and longer: the game's own), and the feet are kept where the game's skin put them, 0.01 m
// above the ground: the result's reference is 0.39 + skin, the start that plus k times 0.005 (the vehicle sweep's
// start: the reference itself). The game's code is not changed beyond its operands: the 22 `fadd / fsub / fmul dword
// [const]` that read these lengths point at a block of four floats of the mod's, which holds the game's values but
// during a call of the sweep; the result's 0.4 (an immediate) is corrected after the call. The block lives in the
// process's heap (a mod that faults is unloaded without its mml_mod_unload, and the game's code must not point into a
// freed DLL); the operands' list is kept with state_put. humanMove and humanUpdate (the sweeps' two callers, both given
// the frame's time) tell the sweeps the time; the fixed 0.1 m probe the game makes while falling or landing (states 1
// and 3) is a probe, not a step: its call keeps the game's lengths.
using HumanFn = void(__fastcall *)(void *, void *, unsigned);   // thiscall(C_human*, unsigned ms), ret 4
using SweepFn = int(__fastcall *)(void *, void *, const float *, const float *, float *);   // (pos, move, out), ret 0xC
HumanFn g_moveOriginal, g_updateOriginal;
SweepFn g_sweepOriginal, g_sweepOwnerOriginal;
bool g_stepHooked;
unsigned g_stepMs;       // the time the sweeps are called with now (0: a caller that does not tell it)
unsigned g_stepsScaled;  // (fps_unlock.status: sweeps made with lengths of a shorter step)
enum { kWalkStart, kWalkAim, kWalkSkin, kWalkOwnerStart, kWalkFloats };
constexpr float kWalkGame[kWalkFloats] = {0.405f, 0.015f, 0.01f, 0.4f};   // the game's values, slot by slot
float *g_walkK;          // the block (the process's heap)
Operand g_walkOps[24];
int g_walkOpsN;
bool g_walkOn;           // the operands point at g_walkK

// The 22 operands, found by their constants in the four functions (tools/check_builds.py checks the same in both
// builds): the sweep's fadd [0.405] and fsub [0.015], the vehicle sweep's fadd [0.4] (its start; its result's fsub
// [0.4] is left alone) and fsub [0.015], the two slides' nine fmul [0.01].
bool FindWalk() {
    size_t n = H->state_get(M, "walkops", g_walkOps, sizeof g_walkOps);
    if (n == sizeof g_walkOps) {   // an earlier load's: each must still be the game's constant or this block
        g_walkOpsN = 22;
        for (int i = 0; i < 22; i++) {
            const Operand &o = g_walkOps[i];
            uint32_t now = 0;
            H->mem_read(o.at, &now, 4);
            if (now != o.original && now != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_walkK[o.slot]))) g_walkOpsN = 0;
            else if (now != o.original) g_walkOn = true;
        }
        if (g_walkOpsN) return true;
    }
    const game::ExeBuild &e = game::Exe();
    struct Where { uintptr_t fn; size_t span; uint8_t op; float value; int slot, want; };   // (span: the function's length)
    const Where where[] = {{e.humanSweep, 0x480, 0x05, 0.405f, kWalkStart, 1}, {e.humanSweep, 0x480, 0x25, 0.015f, kWalkAim, 1},
                           {e.humanSweepOwner, 0x200, 0x05, 0.4f, kWalkOwnerStart, 1},
                           {e.humanSweepOwner, 0x200, 0x25, 0.015f, kWalkAim, 1},
                           {e.humanSlide, 0xA80, 0x0D, 0.01f, kWalkSkin, 9}, {e.humanSlideOwner, 0xA80, 0x0D, 0.01f, kWalkSkin, 9}};
    g_walkOpsN = 0;
    for (const Where &w : where) {
        uint8_t code[0xB00];
        if (!w.fn || !H->mem_read(w.fn, code, w.span)) return false;
        int found = 0;
        for (size_t i = 0; i + 6 <= w.span; i++) {
            if (code[i] != 0xD8 || code[i + 1] != w.op) continue;   // fadd / fsub / fmul dword ptr [abs32]
            uint32_t c;
            float v;
            memcpy(&c, code + i + 2, 4);
            if (!H->mem_read(c, &v, 4) || v != w.value) continue;
            if (g_walkOpsN == 24) return false;
            g_walkOps[g_walkOpsN++] = {w.fn + i + 2, c, w.slot};
            found++;
        }
        if (found != w.want) return false;
    }
    if (g_walkOpsN != 22) return false;
    H->state_put(M, "walkops", g_walkOps, sizeof g_walkOps);
    return true;
}

void Walk(bool on) {
    if (on == g_walkOn || !g_walkOpsN) return;
    for (int i = 0; i < kWalkFloats; i++) g_walkK[i] = kWalkGame[i];
    for (int i = 0; i < g_walkOpsN; i++) {
        uint32_t want = on ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_walkK[g_walkOps[i].slot])) : g_walkOps[i].original;
        H->mem_write(g_walkOps[i].at, &want, 4);   // (game thread: no person is being moved now)
    }
    g_walkOn = on;
}

bool g_stepFall;         // the move is MovementIntern's: its y is the fall (humanUpdate's is an animation's)
unsigned g_stepsGround;  // (fps_unlock.status: of the scaled sweeps, those of a step on the ground)

void __fastcall HumanMove(void *human, void *edx, unsigned ms) {
    const unsigned ms0 = g_stepMs;
    const bool f0 = g_stepFall;
    g_stepMs = ms, g_stepFall = true;
    g_moveOriginal(human, edx, ms);
    g_stepMs = ms0, g_stepFall = f0;
}
void __fastcall HumanUpdate(void *human, void *edx, unsigned ms) {
    const unsigned ms0 = g_stepMs;
    const bool f0 = g_stepFall;
    g_stepMs = ms, g_stepFall = false;
    g_updateOriginal(human, edx, ms);
    g_stepMs = ms0, g_stepFall = f0;
}

// The aim of a step of k * 16 ms. On the ground the fall's speed restarts every frame, so a frame's move goes down
// 9.81 t^2 (the move's y is exactly that): 2.5 mm a 16 ms frame, a fourth length made once a call. Its 16 ms share
// is added to the aim for such a step (in the air the fall goes on from frame to frame and is the time's already).
float AimOf(float k, const float *move) {
    const float t = k * 0.016f, drop = 9.81f * t * t;
    if (!g_stepFall || fabsf(move[1] + drop) > 1e-6f + drop * 1e-3f) return 0.015f * k;
    g_stepsGround++;
    return 0.015f * k + (9.81f * 0.016f * 0.016f * k - drop);
}

// One sweep with the lengths of this call's step.
int Swept(SweepFn sweep, void *human, void *edx, const float *pos, const float *move, float *out) {
    const unsigned ms = g_stepMs;
    if (!g_walkOn || !S.enabled || !S.fixWalk || ms == 0 || ms >= 16) return sweep(human, edx, pos, move, out);
    const int state = *reinterpret_cast<const int *>(static_cast<const char *>(human) + game::Exe().humanMoveState);
    const float len = sqrtf(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
    if ((state == 1 || state == 3) && fabsf(len - 0.1f) < 1e-4f) return sweep(human, edx, pos, move, out);
    const float k = ms / 16.0f, skin = 0.01f * k, reference = 0.39f + skin;
    g_walkK[kWalkStart] = reference + 0.005f * k;
    g_walkK[kWalkAim] = AimOf(k, move);
    g_walkK[kWalkSkin] = skin;
    g_walkK[kWalkOwnerStart] = reference;
    const int hit = sweep(human, edx, pos, move, out);
    for (int i = 0; i < kWalkFloats; i++) g_walkK[i] = kWalkGame[i];
    if (hit) out[1] += 0.4f - reference;   // (the game's result is the sphere's centre minus 0.4)
    g_stepsScaled++;
    return hit;
}
int __fastcall HumanSweep(void *h, void *edx, const float *pos, const float *move, float *out) {
    return Swept(g_sweepOriginal, h, edx, pos, move, out);
}
int __fastcall HumanSweepOwner(void *h, void *edx, const float *pos, const float *move, float *out) {
    return Swept(g_sweepOwnerOriginal, h, edx, pos, move, out);
}

// The player's path, for measuring (fps_unlock.path): every frame, the player's car (its position and speed, as
// game::Car reads them) while he sits in one, else the person's own position (its +0x24, what the sweeps move).
struct PathSample { float t, pos[3], speed, vel[3], extra[3]; bool car; };
int g_pathExtra = -1;   // a C_Vehicle offset whose 3 floats are recorded too ({"start": true, "vehicle_offset": n})
// A block of the car's memory copied every frame, for finding what depends on the step ({"start": true, "dump":
// {"wheel": i} or {"offset": n}, "bytes": n}): one wheel's S_wheel (game::Car::Wheel) or
// the C_Vehicle from an offset. Read back as each float's mean |value| over a window ({"dump_from": s, "dump_to": s}).
int g_dumpWheel = -1, g_dumpOffset = -1, g_dumpBytes = 0;
std::vector<float> g_dump;     // g_dumpBytes / 4 floats per sample, the samples' times in g_dumpT
std::vector<float> g_dumpT;
PathSample g_path[8000];
int g_pathN;
LARGE_INTEGER g_pathStart;

void TracePath() {
    if (!g_pathStart.QuadPart || g_pathN >= static_cast<int>(sizeof g_path / sizeof *g_path)) return;
    void *p = game::Player();
    if (!p) return;
    PathSample s{};
    bool dumped = false;
    if (game::Car car{game::PlayerCar()}) {
        const auto at = car.Position();
        s.pos[0] = at.x, s.pos[1] = at.y, s.pos[2] = at.z;
        s.speed = car.Speed();
        const auto v = car.Velocity();
        s.vel[0] = v.x, s.vel[1] = v.y, s.vel[2] = v.z;
        if (g_pathExtra >= 0) memcpy(s.extra, &car.v<float>(g_pathExtra), sizeof s.extra);
        if (g_dumpBytes > 0 && g_dump.size() < (64u << 20) / sizeof(float)) {
            char *veh = static_cast<char *>(car.Vehicle());
            uintptr_t from = 0;
            if (g_dumpWheel >= 0)
                from = reinterpret_cast<uintptr_t>(car.Wheel(g_dumpWheel));
            else if (g_dumpOffset >= 0)
                from = reinterpret_cast<uintptr_t>(veh) + g_dumpOffset;
            const size_t n = g_dumpBytes / 4, at = g_dump.size();
            g_dump.resize(at + n);
            if (!from || !H->mem_read(from, &g_dump[at], n * 4)) g_dump.resize(at);
            else dumped = true;
        }
        s.car = true;
    } else if (!H->mem_read(reinterpret_cast<uintptr_t>(p) + game::kActorPos, s.pos, sizeof s.pos)) {
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s.t = static_cast<float>((now.QuadPart - g_pathStart.QuadPart) / static_cast<double>(g_freq.QuadPart));
    g_path[g_pathN++] = s;
    if (dumped) g_dumpT.push_back(s.t);
}

// ---- small rotations: LS3D's quaternion-to-matrix made to keep them
// S_quat::RotationMatrix (LS3DF's export, game_build.h quatRotationMatrix: stdcall(const S_quat *q, S_matrix *out)
// -> out) builds the matrix only when |w| < 1 and returns the identity otherwise. A rotation by a small angle a has w = cos(a/2), which is exactly
// 1.0 as a float for any a under about 0.00049 rad, though its vector part is not zero: such a rotation is dropped
// whole. Whoever turns something by a per-frame angle loses every turn slower than 0.00049 rad a frame: 0.03 rad/s
// at 62.5 fps, 0.2 rad/s at 400 fps. A car's body (C_Vehicle::Move turns it each step this way) then cannot yaw a
// little (a dead zone in the steering) nor rock on its springs, so its angular velocity is never answered and a
// stopped car keeps creeping (knowledge/notes/vehicle_physics.md). The fix builds the matrix for |w| = 1 with a
// vector part too, by the same formula (mathx.h QuatMatrix: it is right for every unit quaternion); every other
// quaternion goes to the game's own function, so all that worked before gives the same bits.
using RotMatFn = float *(__stdcall *)(const float *, float *);
RotMatFn g_rotMatOriginal;
bool g_rotMatHooked;
unsigned g_rotMatKept;   // (fps_unlock.status: rotations the game would have dropped)

void BuildRotation(const float *q, float *out) { QuatMatrix(Q{q[0], q[1], q[2], q[3]}, out); }

// Only a unit quaternion whose w rounded to 1 is built here: |w| at most 2 float steps above 1 (a normalized one's
// rounding), its vector part not zero and short (|v|^2 < 1e-6: a turn under 0.002 rad). Anything else (|w| < 1, the
// identity, a quaternion that is not a rotation) is the game's own function's, identity included.
float *__stdcall RotationMatrix(const float *q, float *out) {
    const float w = fabsf(q[0]), v2 = q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!S.enabled || !S.fixRotation || w < 1.0f || w > 1.0f + 2.5e-7f || v2 == 0 || v2 >= 1e-6f)
        return g_rotMatOriginal(q, out);
    g_rotMatKept++;
    BuildRotation(q, out);
    return out;
}

// The formula checked against the game's own function on a quaternion it does build (the decompile it was read
// from could be wrong): the same matrix to float precision, or the fix stays off.
bool RotationFormulaMatches(RotMatFn game) {
    const float n = sqrtf(0.9f * 0.9f + 0.1f * 0.1f + 0.2f * 0.2f + 0.3f * 0.3f);
    const float q[4] = {0.9f / n, 0.1f / n, -0.2f / n, 0.3f / n};
    float a[16], b[16];
    game(q, a);
    BuildRotation(q, b);
    for (int i = 0; i < 16; i++)
        if (fabsf(a[i] - b[i]) > 1e-6f) return false;
    return true;
}

// ---- a car's Tick, hooked for the step probe only
// C_car::AI sets the car's controls and calls C_Vehicle::Tick (game_build.h vehicleTick) with the frame's time; Tick
// runs the drivetrain and hands the time to C_Vehicle::Engine, which moves the car by C_Vehicle::Move in substeps no
// longer than 28 ms (knowledge/notes/vehicle_physics.md). Nothing is changed there: what broke a car above 62.5 fps
// (a stopped car creeping for seconds, the steering's dead zone) was the small rotations lost (above); with them kept,
// the frame's own steps give 62.5 fps's stops and turns (measured at 360 fps: at rest 0.2..0.4 s after slowing down
// against 0.78 s, a yaw rate of 0.161 rad/s for 0.15 of steering against 0.161).
using TickFn = char(__fastcall *)(void *, void *, unsigned);   // thiscall(C_Vehicle*, unsigned ms), ret 4
TickFn g_vehicleTickOriginal;
bool g_vehicleTickHooked;

// ---- the step probe (fps_unlock.stepprobe): what one car's Tick does with one step against several shorter ones,
// from the same state. At the player's car's next Tick: its state is copied (the C_Vehicle, to the C_car's end; each
// wheel, 0x1D4; the body's and the wheels' frames, 0x10..0xB0: matrices, rotation, flags), Tick runs n times with ms, the
// result is kept, the state is put back, Tick runs n2 times with ms2, the state is put back again, and the frame's own
// Tick follows. A step's physics is right when both changes match (to the integration's error); the floats whose
// changes differ most are listed. Crashes (dents), skid marks and sounds made during the probe are not undone: probe
// on a clear road.
struct ProbeReq {
    bool pending;
    unsigned ms, n, ms2, n2;
    std::vector<unsigned> show, logVeh, logWheel;   // logged after every Tick of both runs: vehicle and wheel floats
    std::string result;
} g_probe;
constexpr size_t kWheelBytes = game::kWheelSize, kFrameFrom = 0x10, kFrameBytes = 0xA0;

struct CarCopy {
    std::vector<uint8_t> veh;
    std::vector<std::vector<uint8_t>> wheels, frames;   // frames: the body's, then each wheel's two
    std::vector<char *> framePtrs;
};

void CarSave(game::Car car, size_t vehBytes, CarCopy *c) {
    char *veh = static_cast<char *>(car.Vehicle());
    c->veh.assign(veh, veh + vehBytes);
    c->wheels.clear(), c->frames.clear(), c->framePtrs.clear();
    c->framePtrs.push_back(static_cast<char *>(car.Frame()));
    for (int i = 0; i < car.WheelCount(); i++) {
        char *w = static_cast<char *>(car.Wheel(i));
        if (!w) continue;
        c->wheels.emplace_back(w, w + kWheelBytes);
        c->framePtrs.push_back(game::at<char *>(w, game::kWheelFrame));
        c->framePtrs.push_back(game::at<char *>(w, game::kWheelSteerFrame));
    }
    for (char *f : c->framePtrs)
        c->frames.push_back(f ? std::vector<uint8_t>(f + kFrameFrom, f + kFrameFrom + kFrameBytes) : std::vector<uint8_t>());
}

void CarLoad(game::Car car, const CarCopy &c) {
    memcpy(car.Vehicle(), c.veh.data(), c.veh.size());
    for (size_t i = 0, k = 0; i < static_cast<size_t>(car.WheelCount()) && k < c.wheels.size(); i++)
        if (void *w = car.Wheel(static_cast<int>(i))) memcpy(w, c.wheels[k++].data(), kWheelBytes);
    for (size_t i = 0; i < c.framePtrs.size(); i++)
        if (c.framePtrs[i]) memcpy(c.framePtrs[i] + kFrameFrom, c.frames[i].data(), kFrameBytes);
}

void Probe(void *vehicle, void *edx) {
    char *veh = static_cast<char *>(vehicle);
    game::Car car{veh - game::Exe().vehicle};
    // the C_Vehicle runs to the C_car's end (its last fields are C_car's own: put back with it)
    const size_t kVehBytes = static_cast<size_t>(game::Exe().carSize - game::Exe().vehicle);
    CarCopy start, a, b;
    CarSave(car, kVehBytes, &start);
    // the per-step log: [run, step, vehicle floats..., each wheel's floats...]
    std::string log;
    auto record = [&](int run, unsigned step) {
        if (g_probe.logVeh.empty() && g_probe.logWheel.empty()) return;
        char s[64];
        snprintf(s, sizeof s, "%s[%d,%u", log.empty() ? "" : ",", run, step);
        log += s;
        for (unsigned o : g_probe.logVeh) {
            float x = 0;
            if (o + 4 <= kVehBytes) memcpy(&x, veh + o, 4);
            snprintf(s, sizeof s, ",%.6g", x);
            log += s;
        }
        for (int k = 0; k < car.WheelCount(); k++)
            for (unsigned o : g_probe.logWheel) {
                float x = 0;
                const char *w = static_cast<const char *>(car.Wheel(k));
                if (w && o + 4 <= kWheelBytes) memcpy(&x, w + o, 4);
                snprintf(s, sizeof s, ",%.6g", x);
                log += s;
            }
        log += "]";
    };
    record(0, 0);
    for (unsigned i = 0; i < g_probe.n; i++) g_vehicleTickOriginal(vehicle, edx, g_probe.ms), record(1, i + 1);
    CarSave(car, kVehBytes, &a);
    CarLoad(car, start);
    for (unsigned i = 0; i < g_probe.n2; i++) g_vehicleTickOriginal(vehicle, edx, g_probe.ms2), record(2, i + 1);
    CarSave(car, kVehBytes, &b);
    CarLoad(car, start);
    // every float of the vehicle and the wheels: its change in a and in b
    struct Diff { std::string where; float s, da, db; double score; };
    std::vector<Diff> diffs;
    auto scan = [&](const std::vector<uint8_t> &s0, const std::vector<uint8_t> &s1, const std::vector<uint8_t> &s2, const char *name) {
        for (size_t o = 0; o + 4 <= s0.size(); o += 4) {
            float x0, x1, x2;
            memcpy(&x0, &s0[o], 4), memcpy(&x1, &s1[o], 4), memcpy(&x2, &s2[o], 4);
            if (x0 != x0 || x1 != x1 || x2 != x2 || fabsf(x0) > 1e7f || fabsf(x1) > 1e7f || fabsf(x2) > 1e7f) continue;
            if ((fabsf(x0) < 1e-30f && x0 != 0) || (fabsf(x1) < 1e-30f && x1 != 0)) continue;   // (pointers, flags)
            const float da = x1 - x0, db = x2 - x0, big = fmaxf(fabsf(da), fabsf(db));
            if (big < 1e-6f) continue;
            const double score = fabs(da - db) / big;
            if (score < 0.05) continue;
            char where[48];
            snprintf(where, sizeof where, "%s+0x%X", name, static_cast<unsigned>(o));
            diffs.push_back({where, x0, da, db, score * log10(1 + big * 1000)});
        }
    };
    scan(start.veh, a.veh, b.veh, "veh");
    for (size_t i = 0; i < start.wheels.size(); i++) {
        char name[16];
        snprintf(name, sizeof name, "wheel%u", static_cast<unsigned>(i));
        scan(start.wheels[i], a.wheels[i], b.wheels[i], name);
    }
    std::sort(diffs.begin(), diffs.end(), [](const Diff &x, const Diff &y) { return x.score > y.score; });
    std::string out = "{\"ms\":" + std::to_string(g_probe.ms) + ",\"n\":" + std::to_string(g_probe.n) +
                      ",\"ms2\":" + std::to_string(g_probe.ms2) + ",\"n2\":" + std::to_string(g_probe.n2) + ",\"show\":[";
    for (size_t i = 0; i < g_probe.show.size(); i++) {   // the vehicle's floats asked for, whatever they did
        const unsigned o = g_probe.show[i];
        if (o + 4 > kVehBytes) continue;
        float x0, x1, x2;
        memcpy(&x0, &start.veh[o], 4), memcpy(&x1, &a.veh[o], 4), memcpy(&x2, &b.veh[o], 4);
        char s[128];
        snprintf(s, sizeof s, "%s[\"veh+0x%X\",%.6g,%.6g,%.6g]", i ? "," : "", o, x0, x1 - x0, x2 - x0);
        out += s;
    }
    out += "],\"differ\":[";
    for (size_t i = 0; i < diffs.size() && i < 60; i++) {
        char s[160];
        snprintf(s, sizeof s, "%s[\"%s\",%.6g,%.6g,%.6g]", i ? "," : "", diffs[i].where.c_str(), diffs[i].s, diffs[i].da, diffs[i].db);
        out += s;
    }
    out += "],\"count\":" + std::to_string(diffs.size()) + ",\"log\":[" + log + "]}";
    g_probe.result = out;
}

char __fastcall VehicleTick(void *vehicle, void *edx, unsigned ms) {
    if (g_probe.pending && game::PlayerCar() && static_cast<char *>(vehicle) == static_cast<char *>(game::PlayerCar()) + game::Exe().vehicle) {
        g_probe.pending = false;
        Probe(vehicle, edx);
    }
    return g_vehicleTickOriginal(vehicle, edx, ms);
}

// ---- pacing (a cap of the mod's own: the game's time stays real, the frames are only spaced)
HANDLE g_timer;
LARGE_INTEGER g_next;

void Pace() {
    if (S.maxFps <= 0 || g_owner != Owner::us) { g_next.QuadPart = 0; return; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG period = g_freq.QuadPart / S.maxFps;
    if (!g_next.QuadPart || now.QuadPart - g_next.QuadPart > period) g_next.QuadPart = now.QuadPart;   // (no catching up)
    g_next.QuadPart += period;
    LONGLONG left = g_next.QuadPart - now.QuadPart;
    const LONGLONG spin = g_freq.QuadPart / 2000;   // the last half millisecond: spun, the timer is not that exact
    if (left > spin && g_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -((left - spin) * 10000000 / g_freq.QuadPart);   // 100 ns units, relative
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, 1000);
    }
    do QueryPerformanceCounter(&now); while (now.QuadPart < g_next.QuadPart);
}

// ---- measuring, over each second
struct Window { int frames = 0; double sumMs = 0, maxMs = 0; unsigned dtMin = ~0u, dtMax = 0, dtZero = 0; };
Window g_win, g_last;
// what reached the display over the last second: everything shown and the pictures among it, the pictures that fit
// between two of the game's frames and the display's rate (known false: a loader without gfx_show_stats)
struct Shown { bool known = false, thread = false; double fps = 0, pictures = 0, hz = 0; unsigned fit = 0; } g_shown;
MmlGfxShowStats g_statsAt{};
LARGE_INTEGER g_prev, g_winStart;
unsigned g_prevClock;

void Measure() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_prev.QuadPart) {
        double ms = (now.QuadPart - g_prev.QuadPart) * 1000.0 / g_freq.QuadPart;
        g_win.frames++;
        g_win.sumMs += ms;
        if (ms > g_win.maxMs) g_win.maxMs = ms;
    }
    g_prev = now;
    unsigned clock = 0;
    if (g_clock && H->mem_read(g_clock, &clock, 4)) {
        unsigned dt = clock - g_prevClock;   // the game's step this frame, ms
        if (g_prevClock) {
            if (dt < g_win.dtMin) g_win.dtMin = dt;
            if (dt > g_win.dtMax) g_win.dtMax = dt;
            if (!dt) g_win.dtZero++;
            g_step = dt;
        }
        g_prevClock = clock;
    }
    if (!g_winStart.QuadPart) g_winStart = now;
    if (now.QuadPart - g_winStart.QuadPart >= g_freq.QuadPart) {
        MmlGfxShowStats st{};
        st.size = sizeof st;
        if (MML_HAS(H, gfx_show_stats) && H->gfx_show_stats(M, &st)) {
            const double secs = static_cast<double>(now.QuadPart - g_winStart.QuadPart) / g_freq.QuadPart;
            if (g_statsAt.size) {
                g_shown.fps = static_cast<double>(st.shown - g_statsAt.shown) / secs;
                g_shown.pictures = static_cast<double>(st.pictures - g_statsAt.pictures) / secs;
                g_shown.known = true;
            }
            g_shown.thread = st.thread != 0, g_shown.fit = st.fit, g_shown.hz = st.refresh_hz;
            g_statsAt = st;
        }
        g_last = g_win;
        g_win = Window{};
        g_winStart = now;
    }
}

void OnFrame(const MmlEvent *, void *) {
    Pace();
    Measure();
    StepCam10();   // (this frame's step: the menu loop runs its camera after this event)
    TraceCamera();
    TracePath();
    if (!S.show) return;
    char line[220], shown[80] = "";
    double avg = g_last.frames ? g_last.sumMs / g_last.frames : 0;
    if (g_shown.known && g_shown.thread)   // (frame generation: what the display gets, the pictures that fit now)
        snprintf(shown, sizeof shown, "  shown %.0f fps (%ux at %.0f Hz)", g_shown.fps, g_shown.fit + 1, g_shown.hz);
    snprintf(line, sizeof line, "%d fps  %.1f ms (%.1f max)%s  game step %u..%u ms  %s", g_last.frames, avg, g_last.maxMs, shown,
             g_last.dtMin == ~0u ? 0 : g_last.dtMin, g_last.dtMax,
             g_owner == Owner::us ? "unlocked" : g_owner == Owner::other ? "unlocked by another tool" : "locked");
    H->draw_rect(M, 4, 4, H->draw_text_width(MML_FONT_SMALL, 12, line) + 10, 16, 0x90000000u, 1);
    H->draw_text(M, 9, 5, 12, 0xFFFFE070u, MML_FONT_SMALL, MML_DRAW_LEFT, line);
}

// ---- settings and the hotkey
void OnKey(void *) { H->config_set(M, "enabled", S.enabled ? "false" : "true"); }   // told back as CONFIG_CHANGED
void OnShowKey(void *) { H->config_set(M, "show", S.show ? "false" : "true"); }

void ReadSettings() {
    S.enabled = H->config_number(M, "enabled", 1) != 0;
    S.maxFps = static_cast<int>(H->config_number(M, "max_fps", 0));
    S.show = H->config_number(M, "show", 1) != 0;
    S.fixCamera = H->config_number(M, "fix_menu_camera", 1) != 0;
    S.fixWalk = H->config_number(M, "fix_walk", 1) != 0;
    S.fixRotation = H->config_number(M, "fix_rotation", 1) != 0;
    S.hotkey = H->config_string(M, "hotkey", "F11");
    H->hotkey(M, S.hotkey.c_str(), OnKey, nullptr);
    S.showHotkey = H->config_string(M, "show_hotkey", "Shift+F11");
    H->hotkey(M, S.showHotkey.c_str(), OnShowKey, nullptr);
    if (S.enabled) Apply();
    else Restore();
    Cam10(S.enabled && S.fixCamera);
    Walk(S.enabled && S.fixWalk && g_stepHooked);
}

void OnConfig(const MmlEvent *, void *) { ReadSettings(); }

int Status(const char *, MmlReply *r, void *) {
    char out[1200];
    double avg = g_last.frames ? g_last.sumMs / g_last.frames : 0;
    snprintf(out, sizeof out,
             "{\"build\":\"%s\",\"known\":%s,\"enabled\":%s,\"unlocked\":\"%s\",\"why\":\"%s\",\"max_fps\":%d,"
             "\"fps\":%d,\"frame_ms\":%.2f,\"frame_ms_max\":%.2f,\"game_step_ms\":[%u,%u],\"game_steps_zero\":%u,"
             "\"menu_camera\":\"%s\",\"walk\":\"%s\",\"walk_sweeps_scaled\":[%u,%u],\"car_tick\":\"%s\",\"rotation\":\"%s\",\"rotations_kept\":%u,"
             "\"shown_fps\":%.1f,\"pictures_fps\":%.1f,\"showing_thread\":%s,\"fit\":%u,\"refresh_hz\":%.1f}",
             H->game_build(), g_known ? "true" : "false", S.enabled ? "true" : "false",
             g_owner == Owner::us ? "by this mod" : g_owner == Owner::other ? "by another tool" : "no", g_why, S.maxFps,
             g_last.frames, avg, g_last.maxMs, g_last.dtMin == ~0u ? 0 : g_last.dtMin, g_last.dtMax, g_last.dtZero,
             g_camHooked ? (S.enabled && S.fixCamera ? "fixed: TickCamera hooked" : "TickCamera hooked, fix off")
             : g_cam10 ? "fixed: the loop's operands on the mod's coefficients"
             : g_ops10N ? "found, fix off" : "not known for this build",
             g_stepHooked && g_walkOpsN ? (g_walkOn ? "fixed: the sweep's lengths made of the step's time" : "hooked, fix off")
             : game::g_exe && game::Exe().humanSweep ? "its code or hooks not as expected" : "not known for this build",
             g_stepsScaled, g_stepsGround,
             g_vehicleTickHooked ? "hooked (the step probe only)" : "not known for this build",
             g_rotMatHooked ? (S.enabled && S.fixRotation ? "fixed: small rotations kept" : "hooked, fix off") : "not hooked",
             g_rotMatKept, g_shown.fps, g_shown.pictures, g_shown.thread ? "true" : "false", g_shown.fit, g_shown.hz);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
// fps_unlock.camera {"start": true}: the scene camera's path recorded from now on; without it: how the path went.
// The way is from the first sample to the last (the camera has come to rest there when the recording is long enough;
// the menu camera's slow sway is in both), and the times are from the start, when the camera was told where to go.
int Camera(const char *args, MmlReply *r, void *) {
    if (args && strstr(args, "\"start\":true")) {
        g_traceN = 0;
        g_traceLast.QuadPart = 0;
        QueryPerformanceCounter(&g_traceStart);
        H->reply_append(r, "{\"recording\":true}", static_cast<size_t>(-1));
        return 0;
    }
    auto dist = [](const float *a, const float *b) {
        float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    };
    double at[3] = {-1, -1, -1};
    const float marks[3] = {0.1f, 0.5f, 0.9f};
    float way = g_traceN > 1 ? dist(g_trace[0].pos, g_trace[g_traceN - 1].pos) : 0.0f;
    for (int i = 0; i < g_traceN && way > 1e-4f; i++)
        for (int m = 0; m < 3; m++)
            if (at[m] < 0 && 1.0f - dist(g_trace[i].pos, g_trace[g_traceN - 1].pos) / way >= marks[m]) at[m] = g_trace[i].t;
    char head[320];
    snprintf(head, sizeof head, "{\"fix\":%s,\"samples\":%d,\"way\":%.3f,\"s_to_10\":%.3f,\"s_to_50\":%.3f,\"s_to_90\":%.3f,\"path\":[",
             S.enabled && S.fixCamera ? "true" : "false", g_traceN, way, at[0], at[1], at[2]);
    std::string out = head;
    for (int i = 0, n = 0; i < g_traceN; i += 25, n++) {   // every 25th sample: about 2 a second, [s, x, y, z]
        char s[96];
        snprintf(s, sizeof s, "%s[%.2f,%.3f,%.3f,%.3f]", n ? "," : "", g_trace[i].t, g_trace[i].pos[0], g_trace[i].pos[1], g_trace[i].pos[2]);
        out += s;
    }
    out += "]}";
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}
int StepProbe(const char *args, MmlReply *r, void *) {
    auto number = [&](const char *key, unsigned otherwise) {
        const char *at = args ? strstr(args, key) : nullptr;
        return at ? static_cast<unsigned>(atoi(at + strlen(key))) : otherwise;
    };
    if (args && strstr(args, "\"ms\":")) {
        g_probe.ms = number("\"ms\":", 16), g_probe.n = number("\"n\":", 1);
        g_probe.ms2 = number("\"ms2\":", 4), g_probe.n2 = number("\"n2\":", 4);
        auto list = [&](const char *key, std::vector<unsigned> *to) {   // {"show": [800, 8080, ...]}: decimal offsets
            to->clear();
            const char *at = strstr(args, key);
            if (!at) return;
            for (const char *p = at + strlen(key); *p && *p != ']';) {
                char *end;
                long o = strtol(p, &end, 0);
                if (end == p) { p++; continue; }
                to->push_back(static_cast<unsigned>(o));
                p = end;
            }
        };
        list("\"show\":[", &g_probe.show);
        list("\"log_veh\":[", &g_probe.logVeh);
        list("\"log_wheel\":[", &g_probe.logWheel);
        g_probe.result.clear();
        g_probe.pending = true;
        H->reply_append(r, "{\"pending\":true}", static_cast<size_t>(-1));
        return 0;
    }
    const std::string &s = g_probe.result.empty() ? std::string(g_probe.pending ? "{\"pending\":true}" : "{}") : g_probe.result;
    H->reply_append(r, s.c_str(), s.size());
    return 0;
}

// fps_unlock.sweepprobe {"ms": 4, "s": 1.5, "speed": 1.7}: the person's collision sweep (humanSweep) called as a walk
// would call it, from where the player stands, facing where he faces, in steps of ms for s seconds, and everything put
// back after (the player's frame and position): the feet's path [t, x, y, z] every 16 ms of it. The sweep's lengths
// are the game's, or with "fix": true those fix_walk gives a step of ms, or any given: "start" (the sphere's start
// above the feet, 0.405), "aim" (how much lower than the move it is aimed, 0.015), "skin" (how far off a contact each
// slide starts again, 0.01), "out" (the result's offset below the sphere's centre, 0.4). The fall is the walk's: 9.81 t
// a step added to the fall's speed, the speed restarted on every touch. How the fix was found (knowledge/notes/game.md).
int SweepProbe(const char *args, MmlReply *r, void *) {
    char *p = static_cast<char *>(game::Player());
    if (!p || game::PlayerCar()) return H->reply_error(r, "the player on foot"), 0;
    if (!g_walkOpsN || !g_stepHooked) return H->reply_error(r, "the sweep's lengths are not known for this build"), 0;
    auto number = [&](const char *key, double otherwise) {
        const char *at = args ? strstr(args, key) : nullptr;
        return at ? atof(at + strlen(key)) : otherwise;
    };
    const float ms = static_cast<float>(number("\"ms\":", 16)), seconds = static_cast<float>(number("\"s\":", 1.5));
    const float speed = static_cast<float>(number("\"speed\":", 1.7)), dt = ms * 0.001f;
    if (ms <= 0 || seconds <= 0 || seconds / dt > 20000) return H->reply_error(r, "ms, s: at most 20000 steps"), 0;
    const bool fix = args && strstr(args, "\"fix\":true");
    const float k = fix && ms < 16 ? ms / 16.0f : 1.0f, reference = 0.39f + 0.01f * k;
    float use[kWalkFloats] = {fix ? reference + 0.005f * k : 0.405f, 0.015f * k, 0.01f * k, reference};
    use[kWalkStart] = static_cast<float>(number("\"start\":", use[kWalkStart]));
    const bool aimGiven = args && strstr(args, "\"aim\":");   // (else with "fix": the fix's aim, step by step)
    use[kWalkAim] = static_cast<float>(number("\"aim\":", use[kWalkAim]));
    use[kWalkSkin] = static_cast<float>(number("\"skin\":", use[kWalkSkin]));
    const float outK = static_cast<float>(number("\"out\":", fix ? reference : 0.4f));
    const bool wasOn = g_walkOn;
    Walk(true);
    char *frame = game::at<char *>(p, game::kActorFrame);
    uint8_t keep[0xA0], keepPos[12];
    memcpy(keep, frame + 0x10, sizeof keep);
    memcpy(keepPos, p + game::kActorPos, sizeof keepPos);
    float feet[3];
    memcpy(feet, frame + game::kFrameWorld + 48, sizeof feet);
    const V3 facing = Norm(game::at<V3>(p, game::kActorFacing));
    std::string out = "{\"path\":[";
    char s[96];
    float fall = 0, next = 0;
    int hits = 0, n = 0;
    for (float t = 0; t < seconds - 1e-6f; t += dt) {
        if (t >= next - 1e-6f) {
            snprintf(s, sizeof s, "%s[%.3f,%.4f,%.4f,%.4f]", n++ ? "," : "", t, feet[0], feet[1], feet[2]);
            out += s, next += 0.016f;
        }
        fall += 9.81f * dt;
        const float move[3] = {facing.x * speed * dt, -fall * dt, facing.z * speed * dt};
        memcpy(frame + game::kFrameWorld + 48, feet, sizeof feet);   // (the sweep starts from the frame's world place)
        game::at<uint32_t>(frame, game::kFrameFlags) |= game::kFlagWorldValid;
        memcpy(p + game::kActorPos, feet, sizeof feet);
        float to[3];
        if (fix && !aimGiven) {
            const bool f0 = g_stepFall;
            g_stepFall = true;
            use[kWalkAim] = AimOf(k, move);
            g_stepFall = f0;
        }
        memcpy(g_walkK, use, sizeof use);
        const int hit = g_sweepOriginal(p, nullptr, feet, move, to);
        for (int i = 0; i < kWalkFloats; i++) g_walkK[i] = kWalkGame[i];
        if (hit) hits++, fall = 0, to[1] += 0.4f - outK;
        memcpy(feet, to, sizeof feet);
    }
    memcpy(frame + 0x10, keep, sizeof keep);
    memcpy(p + game::kActorPos, keepPos, sizeof keepPos);
    Walk(wasOn);
    snprintf(s, sizeof s, "%s[%.3f,%.4f,%.4f,%.4f]],\"hits\":%d,\"steps\":%d}", n ? "," : "", seconds, feet[0], feet[1],
             feet[2], hits, static_cast<int>(seconds / dt + 0.5f));
    out += s;
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}

// fps_unlock.path {"start": true}: the player's path recorded every frame from now on (8000 frames at most).
// Without it: the path every `every_ms` (default 50) as [s, x, y, z] on foot, [s, x, y, z, speed m/s, [velocity],
// [the 3 floats at `vehicle_offset`]] in a car; `s_up`, when the
// player first stood 5 cm higher than at the start (a curb climbed); `s_still`, from when on every sample stays
// within 1 mm of the last (a car come to rest), and `still_m`, how far it went after its speed first fell below
// 0.1 m/s (a stop's creep).
int Path(const char *args, MmlReply *r, void *) {
    if (args && strstr(args, "\"start\":true")) {
        g_pathN = 0;
        auto number = [&](const char *key, int otherwise) {
            const char *at = strstr(args, key);
            return at ? atoi(at + strlen(key)) : otherwise;
        };
        g_pathExtra = number("\"vehicle_offset\":", -1);
        g_dumpWheel = number("\"dump_wheel\":", -1);
        g_dumpOffset = number("\"dump_offset\":", -1);
        g_dumpBytes = number("\"dump_bytes\":", 0) & ~3;
        g_dump.clear();
        g_dumpT.clear();
        QueryPerformanceCounter(&g_pathStart);
        H->reply_append(r, "{\"recording\":true}", static_cast<size_t>(-1));
        return 0;
    }
    if (const char *from = args ? strstr(args, "\"dump_from\":") : nullptr) {   // the dump's means over a window
        const char *to = strstr(args, "\"dump_to\":");
        const float t0 = static_cast<float>(atof(from + 12)), t1 = to ? static_cast<float>(atof(to + 10)) : 1e9f;
        const size_t n = g_dumpBytes / 4;
        std::vector<double> sumAbs(n), sum(n);
        int count = 0;
        for (size_t k = 0; k < g_dumpT.size(); k++) {
            if (g_dumpT[k] < t0 || g_dumpT[k] > t1) continue;
            for (size_t f = 0; f < n; f++) {
                const float x = g_dump[k * n + f];
                if (x != x || fabsf(x) > 1e7f) continue;   // (not a float: pointers, flags)
                sumAbs[f] += fabsf(x), sum[f] += x;
            }
            count++;
        }
        // {"dump_keep": true}: these means kept as the reference; a later read lists only the fields more than 10 %
        // away from it (by |value|), as [offset, mean |value|, the reference's, their ratio, mean value]
        static std::vector<double> reference;
        const bool keep = strstr(args, "\"dump_keep\":true") != nullptr;
        std::string out = "{\"frames\":" + std::to_string(count) + ",\"fields\":[";
        int listed = 0;
        for (size_t f = 0; f < n && count; f++) {
            const double now = sumAbs[f] / count, ref = f < reference.size() ? reference[f] : -1;
            if (keep || (ref >= 0 && (fabs(now - ref) <= 0.1 * fmax(fabs(now), fabs(ref)) || fmax(now, ref) < 1e-6))) continue;
            char s[128];
            snprintf(s, sizeof s, "%s[\"0x%X\",%.5g,%.5g,%.3g,%.5g]", listed++ ? "," : "",
                     static_cast<unsigned>(f * 4 + (g_dumpWheel < 0 ? g_dumpOffset : 0)), now, ref,
                     ref > 1e-9 ? now / ref : 0.0, sum[f] / count);
            out += s;
        }
        out += "]}";
        if (keep) {
            reference.assign(n, 0);
            for (size_t f = 0; f < n && count; f++) reference[f] = sumAbs[f] / count;
        }
        H->reply_append(r, out.c_str(), out.size());
        return 0;
    }
    float every = 0.05f;
    if (const char *e = args ? strstr(args, "\"every_ms\":") : nullptr) every = static_cast<float>(atof(e + 11)) / 1000.0f;
    auto dist = [](const float *a, const float *b) {
        float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    };
    double up = -1, still = -1, slow = -1;
    float creep = 0;
    const PathSample *last = g_pathN ? &g_path[g_pathN - 1] : nullptr;
    for (int i = 0; i < g_pathN && up < 0; i++)
        if (g_path[i].pos[1] > g_path[0].pos[1] + 0.05f) up = g_path[i].t;
    for (int i = g_pathN - 1; i >= 0 && dist(g_path[i].pos, last->pos) <= 0.001f; i--) still = g_path[i].t;
    for (int i = 1; i < g_pathN; i++) {
        if (slow < 0 && g_path[i].car && g_path[i].speed < 0.1f && g_path[i - 1].speed >= 0.1f) slow = g_path[i].t;
        if (slow >= 0) creep += dist(g_path[i].pos, g_path[i - 1].pos);
    }
    char head[256];
    snprintf(head, sizeof head, "{\"fix_walk\":%s,\"samples\":%d,\"s_up\":%.3f,\"s_still\":%.3f,\"s_slow\":%.3f,\"still_m\":%.4f,\"path\":[",
             S.enabled && S.fixWalk ? "true" : "false", g_pathN, up, still, slow, creep);
    std::string out = head;
    float next = 0;
    for (int i = 0, n = 0; i < g_pathN; i++) {
        if (g_path[i].t < next && i != g_pathN - 1) continue;
        const PathSample &p = g_path[i];
        char s[256];
        if (p.car)
            snprintf(s, sizeof s, "%s[%.3f,%.4f,%.4f,%.4f,%.4f,[%.4f,%.4f,%.4f],[%.4f,%.4f,%.4f]]", n++ ? "," : "", p.t,
                     p.pos[0], p.pos[1], p.pos[2], p.speed, p.vel[0], p.vel[1], p.vel[2], p.extra[0], p.extra[1], p.extra[2]);
        else
            snprintf(s, sizeof s, "%s[%.3f,%.4f,%.4f,%.4f]", n++ ? "," : "", p.t, p.pos[0], p.pos[1], p.pos[2]);
        out += s;
        next = g_path[i].t + every;
    }
    out += "]}";
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, config_set)) return 1;   // a loader older than the mod kit
    QueryPerformanceFrequency(&g_freq);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);   // (Windows before 10 1803: coarser)
    g_known = Find();
    host->state_get(mod, "k10", &g_k10, sizeof g_k10);   // 1.0's camera floats: the process's heap, one block for every load
    if (!g_k10) {
        g_k10 = static_cast<float *>(HeapAlloc(GetProcessHeap(), 0, 2 * sizeof(float)));
        if (g_k10) g_k10[0] = 0.001f, g_k10[1] = 0.002f;
        host->state_put(mod, "k10", &g_k10, sizeof g_k10);
    }
    if (g_known && g_k10 && game::Exe().menuCamLoop && !FindCam10()) {
        g_ops10N = 0;
        host->log(mod, MML_LOG_WARN, "1.0's menu camera: its loop is not as expected; left as it is");
    }
    if (!g_known) host->log(mod, MML_LOG_WARN, "%s: the game's pacing is left as it is", g_why);
    if (game::g_exe && game::Exe().humanSweep) {   // (hooks are taken off with the mod; one that failed leaves the
        const game::ExeBuild &e = game::Exe();       //  others calling straight through: no time, no change)
        auto hook = [&](uintptr_t at, auto fn, auto *original) {
            return host->hook(mod, at, reinterpret_cast<void *>(fn), reinterpret_cast<void **>(original)) != 0;
        };
        if (e.vehicleTick) g_vehicleTickHooked = hook(e.vehicleTick, VehicleTick, &g_vehicleTickOriginal);
        // LS3D's quaternion-to-matrix (an export of LS3DF: game_build.h has it for both builds)
        const uintptr_t ls3d = host->module_base("LS3DF.dll");
        auto rotMat = ls3d && game::g_ls3d && game::Ls3d().quatRotationMatrix
                          ? reinterpret_cast<RotMatFn>(ls3d + game::Ls3d().quatRotationMatrix) : nullptr;
        if (rotMat && RotationFormulaMatches(rotMat))
            g_rotMatHooked = hook(reinterpret_cast<uintptr_t>(rotMat), RotationMatrix, &g_rotMatOriginal);
        else
            host->log(mod, MML_LOG_WARN, "small rotations: S_quat::RotationMatrix %s; left as it is",
                      rotMat ? "does not compute as expected" : "not found");
        g_stepHooked = hook(e.humanSweep, HumanSweep, &g_sweepOriginal) &&
                       hook(e.humanSweepOwner, HumanSweepOwner, &g_sweepOwnerOriginal) &&
                       hook(e.humanMove, HumanMove, &g_moveOriginal) && hook(e.humanUpdate, HumanUpdate, &g_updateOriginal);
        if (!g_stepHooked) host->log(mod, MML_LOG_WARN, "a person's step: its functions could not be hooked; left as it is");
        host->state_get(mod, "walkk", &g_walkK, sizeof g_walkK);   // the sweep's lengths: the process's heap, as k10
        if (!g_walkK) {
            g_walkK = static_cast<float *>(HeapAlloc(GetProcessHeap(), 0, kWalkFloats * sizeof(float)));
            if (g_walkK) memcpy(g_walkK, kWalkGame, sizeof kWalkGame);
            host->state_put(mod, "walkk", &g_walkK, sizeof g_walkK);
        }
        if (g_stepHooked && (!g_walkK || !FindWalk())) {
            g_walkOpsN = 0;
            host->log(mod, MML_LOG_WARN, "a person's step: the sweep's lengths are not where expected; left as they are");
        }
    }
    ReadSettings();
    if (game::g_exe && game::Exe().menuCamTick)
        g_camHooked = host->hook(mod, game::Exe().menuCamTick, reinterpret_cast<void *>(CamTick),
                                 reinterpret_cast<void **>(&g_camOriginal)) != 0;   // (taken off with the mod)
    host->command(mod, "camera", "{start: true}: the main menu camera's eye recorded; then its path", Camera, nullptr);
    host->command(mod, "stepprobe", "{ms, n, ms2, n2}: the player's car stepped n x ms and n2 x ms2 from one state, put back; then {} reads what differed",
                  StepProbe, nullptr);
    host->command(mod, "sweepprobe", "{ms, s, speed, fix, start, aim, skin, out}: the person's sweep called as a walk would from where the player stands, put back after; the feet's path", SweepProbe, nullptr);
    host->command(mod, "path","{start: true}: the player's (or his car's) position recorded every frame; then its path {every_ms}", Path, nullptr);
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnConfig, nullptr);
    host->command(mod, "status", "unlocked or not, frames a second, the frame's time, the game's clock step", Status, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    Cam10(false);   // (the game's code back to its own constants)
    Walk(false);
    Restore();   // the game as it was: a reload patches again, a mod switched off leaves the game's own pacing
    if (g_timer) CloseHandle(g_timer);
    g_timer = nullptr;
}
