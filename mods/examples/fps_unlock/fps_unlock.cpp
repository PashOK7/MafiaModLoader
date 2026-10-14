// fps_unlock: the frame rate freed, the game's time kept real (MafiaD3D11's A132, as a mod of the loader).
//
// How the game paces itself: its main loop asks LS3D's frame limiter (IGraph::GetTimer, `frameLimit` in
// game_build.h) for a frame of at least 16 ms and at most 83 ms. A shorter frame is slept to 16 ms, so the game never
// runs above 62.5 fps; the limiter's answer is the time every system of the game is given (the mission's tick, the
// recordings, the caches, the map, the cutscenes). Present also yields (Sleep(0)) before each frame.
// Two places changed, nothing else: the limiter's `jae` becomes `jmp` (it never waits and returns the frame's real
// time, in whole milliseconds; a long frame is still cut to 83 ms), and Present's `push 0; call Sleep` is jumped
// over. The game's time stays real: at 200 fps every system gets 5 ms a frame instead of 16 ms every 16 ms.
// What the game did once a frame instead of by the time breaks then; one is fixed here (the main menu's camera,
// CamTick) and the others found go to knowledge/notes/game.md, "The frame rate".
//
// What it shows a mod author:
//   patching the game's code   Find, Apply, Restore   the build's addresses from mml/game/game_build.h; the bytes
//                              checked before writing (another tool may have changed them: then nothing is done);
//                              written on the game thread (mods load and get their events there, so no thread runs
//                              that code meanwhile); put back at the unload (a hot reload or the mod switched off
//                              leave the game as it was)
//   fixing a game function     CamTick       a hook (host->hook: chained, taken off with the mod) that calls the
//                              game's own function and corrects what it did once a frame
//   pacing frames precisely    Pace         a frame-rate cap of its own, on a high-resolution waitable timer (the
//                              game's Sleep has a 1 ms grain at best)
//   measuring the frame        Measure       frames a second, the frame's time, the game's own clock step
//   a setting switched live    OnKey, ReadSettings   the hotkey flips "enabled" with config_set; the change comes back
//                              as MML_EVENT_CONFIG_CHANGED like an edit of the file or the bridge's config.set
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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
    std::string hotkey = "F11";
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
    if (!S.show) return;
    char line[160];
    double avg = g_last.frames ? g_last.sumMs / g_last.frames : 0;
    snprintf(line, sizeof line, "%d fps  %.1f ms (%.1f max)  game step %u..%u ms  %s", g_last.frames, avg, g_last.maxMs,
             g_last.dtMin == ~0u ? 0 : g_last.dtMin, g_last.dtMax,
             g_owner == Owner::us ? "unlocked" : g_owner == Owner::other ? "unlocked by another tool" : "locked");
    H->draw_rect(M, 4, 4, H->draw_text_width(MML_FONT_SMALL, 12, line) + 10, 16, 0x90000000u, 1);
    H->draw_text(M, 9, 5, 12, 0xFFFFE070u, MML_FONT_SMALL, MML_DRAW_LEFT, line);
}

// ---- settings and the hotkey
void OnKey(void *) { H->config_set(M, "enabled", S.enabled ? "false" : "true"); }   // told back as CONFIG_CHANGED

void ReadSettings() {
    S.enabled = H->config_number(M, "enabled", 1) != 0;
    S.maxFps = static_cast<int>(H->config_number(M, "max_fps", 0));
    S.show = H->config_number(M, "show", 1) != 0;
    S.fixCamera = H->config_number(M, "fix_menu_camera", 1) != 0;
    S.hotkey = H->config_string(M, "hotkey", "F11");
    H->hotkey(M, S.hotkey.c_str(), OnKey, nullptr);
    if (S.enabled) Apply();
    else Restore();
    Cam10(S.enabled && S.fixCamera);
}

void OnConfig(const MmlEvent *, void *) { ReadSettings(); }

int Status(const char *, MmlReply *r, void *) {
    char out[512];
    double avg = g_last.frames ? g_last.sumMs / g_last.frames : 0;
    snprintf(out, sizeof out,
             "{\"build\":\"%s\",\"known\":%s,\"enabled\":%s,\"unlocked\":\"%s\",\"why\":\"%s\",\"max_fps\":%d,"
             "\"fps\":%d,\"frame_ms\":%.2f,\"frame_ms_max\":%.2f,\"game_step_ms\":[%u,%u],\"game_steps_zero\":%u,"
             "\"menu_camera\":\"%s\"}",
             H->game_build(), g_known ? "true" : "false", S.enabled ? "true" : "false",
             g_owner == Owner::us ? "by this mod" : g_owner == Owner::other ? "by another tool" : "no", g_why, S.maxFps,
             g_last.frames, avg, g_last.maxMs, g_last.dtMin == ~0u ? 0 : g_last.dtMin, g_last.dtMax, g_last.dtZero,
             g_camHooked ? (S.enabled && S.fixCamera ? "fixed: TickCamera hooked" : "TickCamera hooked, fix off")
             : g_cam10 ? "fixed: the loop's operands on the mod's coefficients"
             : g_ops10N ? "found, fix off" : "not known for this build");
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
    ReadSettings();
    if (game::g_exe && game::Exe().menuCamTick)
        g_camHooked = host->hook(mod, game::Exe().menuCamTick, reinterpret_cast<void *>(CamTick),
                                 reinterpret_cast<void **>(&g_camOriginal)) != 0;   // (taken off with the mod)
    host->command(mod, "camera", "{start: true}: the main menu camera's eye recorded; then its path", Camera, nullptr);
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnConfig, nullptr);
    host->command(mod, "status", "unlocked or not, frames a second, the frame's time, the game's clock step", Status, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    Cam10(false);   // (before the DLL goes: the game's code points into it)
    Restore();   // the game as it was: a reload patches again, a mod switched off leaves the game's own pacing
    if (g_timer) CloseHandle(g_timer);
    g_timer = nullptr;
}
