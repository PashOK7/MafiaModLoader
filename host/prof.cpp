// Measuring the frame (docs/MODDING.md "Measuring the frame"): what each mod costs and where the game thread's time
// goes, for agents and mod authors (a water or VR mod lives within a frame's budget: 11 ms at 90 Hz).
//
// 1. Every entry into a mod is timed where the host makes it (events, commands, passes, views' functions, hotkeys,
//    menus' and scripts' callbacks, posted calls): its mod and its site ("RENDER_WORLD", "its graphics pass"...),
//    summed a frame, a frame being Present to Present. The host's own heavy parts are timed too (the scene views, the
//    graphics bridge), their mods' calls inside them. Frames are kept in a ring: the frame time's average, median,
//    95th percentile and worst; a frame much longer than the median is a hitch, kept with what took its time.
// 2. A sampling profiler for the game thread (prof.sample): another thread stops it every half millisecond and reads
//    where it is (EIP), what it was doing (the phase: the game's own work, LS3D drawing the frame, the scene views,
//    the graphics bridge), inside which mod's call, and, when it is in a system or driver module, the first return
//    address on its stack into the game's code (who called the driver). The report is by module, by mod, by phase,
//    by address (16-byte buckets) and by caller, as "Game.exe+0x..." (MCP's `profile` names them).
//
// Bridge methods: prof.state, prof.reset (game thread); prof.sample {seconds} -> {id}, prof.sample {id} -> the report.
#include "host.h"
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "mml/game/game_build.h"
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

double NowMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

// ---- the sites
struct Stat {
    std::string mod, site;      // mod "host": the host's own part
    double frameMs = 0;         // this frame
    unsigned frameCalls = 0;
    double totalMs = 0, maxMs = 0;   // since the reset: summed over frames, the worst frame
    double ema = 0;                  // a frame's, a moving average (frame_stats)
    unsigned calls = 0, framesWith = 0;
};
std::map<std::pair<std::string, std::string>, Stat> g_stats;
unsigned g_frames;              // frames since the reset
double g_resetAt;

// the frames' times: a ring
constexpr size_t kRing = 600;
float g_ring[kRing];
size_t g_ringN, g_ringAt;
double g_lastPresent;

// ---- each frame's record (prof.frames): a frame is from one return of the game's Present to the next
// The time stamp counter is the clock of the costly sites and the device's calls (a few cycles a read); its rate is
// measured against QueryPerformanceCounter over the first half second and kept up to date after.
uint64_t g_tsc0;
double g_tscMs0, g_tscPerMs;
void TscTrack() {
    uint64_t t = __rdtsc();
    double ms = NowMs();
    if (!g_tsc0) { g_tsc0 = t, g_tscMs0 = ms; return; }
    if (ms - g_tscMs0 >= 500) g_tscPerMs = static_cast<double>(t - g_tsc0) / (ms - g_tscMs0);
}
// The game thread's own cycles (a call into the kernel, about a microsecond: twice a frame). The process's, each frame,
// cost 2 % of a 2.8 ms frame (QueryProcessCycleTime walks every thread): MCP's `bench` takes the process's over a run.
uint64_t ThreadCycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

enum FrameFlag : uint8_t {
    kFlagSettling = 1,        // after a mark: the frames let pass before the label's own
    kFlagPresentNotOk = 2,    // Present did not answer S_OK (occluded: S_PRESENT_OCCLUDED, a lost device...)
    kFlagCounted = 4,         // the device's calls were counted (prof.counters)
    kFlagSampled = 8,         // the sampler stopped the game thread meanwhile (prof.sample)
};
struct FrameRec {
    uint32_t seq;
    uint16_t label;           // g_labels' index (0: none)
    uint8_t flags;
    float wallMs, presentMs, presentCpuMs, cpuMs, gpuMs;   // gpuMs NaN until known
    float hotMs[kHotCount];
    D3dFrameCounts d3d;
};
constexpr size_t kFrames = 4096;
FrameRec *g_frameRing;        // (allocated at the first frame: 4096 records)
uint32_t g_seq = 1;           // the frame being made
double g_frameStartMs, g_presentStartMs;
uint64_t g_frameThreadC, g_presentStartC;
float g_presentMs, g_presentCpuMs;
long g_presentHr;
std::vector<std::string> g_labels{""};
uint16_t g_label, g_nextLabel;
int g_settleLeft;
std::atomic<bool> g_profHadSample{false};
FrameRec *RecOf(uint32_t seq) {
    if (!g_frameRing || !seq || seq >= g_seq || g_seq - seq > kFrames) return nullptr;
    FrameRec *r = &g_frameRing[seq % kFrames];
    return r->seq == seq ? r : nullptr;
}

struct Hitch { double atS, ms, median; json parts; };
std::vector<Hitch> g_hitches;

// the mod being called now (for the sampler): its index in g_names, -1 none; a stack for nesting
std::vector<std::string> g_names;
std::atomic<int> g_curMod{-1};
std::atomic<int> g_phase{0};
int g_depth;
int g_stack[16];
int g_phaseStack[16];
int g_phaseDepth;

int NameIndex(const std::string &id) {
    if (!g_names.capacity()) g_names.reserve(256);   // (never moved: the sampler's thread reads it)
    for (size_t i = 0; i < g_names.size(); i++) if (g_names[i] == id) return static_cast<int>(i);
    if (g_names.size() >= 250) return -1;
    g_names.push_back(id);
    return static_cast<int>(g_names.size() - 1);
}

const char *EventName(int type) {
    static const char *names[] = {"?", "FRAME", "MISSION_TICK", "MISSION_CLOSE", "ACTOR_DESTROYED", "HUMAN_CREATED", "HUMAN_HIT",
                                  "SERVICES_CHANGED", "MOD_LOADED", "MISSION_OPEN", "MISSION_LOADED", "GAME_PAUSED", "GAME_RESUMED",
                                  "CONFIG_CHANGED", "FILE_CHANGED", "RENDER_BEGIN", "RENDER_WORLD", "RENDER_END", "PRESENT", "VIEW_WORLD",
                                  "SHOT", "EXPLOSION", "HUMAN_DIED", "VEHICLE_ENTERED", "VEHICLE_LEFT", "CAR_EXPLODED",
                                  "GAME_SAVING", "GAME_LOADED", "PEDESTRIAN_HIT", "BULLET_HIT", "CAR_CRASH", "HUMAN_FELL",
                                  "ITEM_GAINED", "ITEM_LOST", "MONEY_CHANGED"};
    static_assert(sizeof names / sizeof names[0] == MML_EVENT_COUNT_, "a name for every MML_EVENT_*");
    return type >= 0 && type < static_cast<int>(sizeof names / sizeof names[0]) ? names[type] : "event";
}

// ---- the sampler
const char *kPhaseNames[] = {"game", "frame drawing", "scene views", "graphics bridge"};
constexpr int kPhases = 4;
// A sample: where the game thread was (EIP), the game's code that called the system or driver module it was in
// (caller), the host's or a mod's code that did (host: the first of theirs above the game's on the stack), and the
// chain of return addresses into the game's, the host's and the mods' code found on its stack (g_chain[chainAt..]).
struct Sample { uint32_t eip, caller, host, chainAt; uint16_t chainN; int16_t mod; uint8_t phase; };
std::vector<Sample> g_samples;
std::vector<uint32_t> g_chain;
std::atomic<bool> g_sampling{false};
int g_sampleId, g_nextSampleId = 1;
double g_sampleSeconds;
std::mutex g_reportMx;
json g_report;                  // the last finished run's (with its id)

struct Mod { uintptr_t base, end; std::string name; CodeKind kind; };   // ours: the loader and the mods (mml\live)
std::vector<Mod> g_modules;     // read before a run, by base: no module is looked up while the game thread is stopped

void ReadModules() {
    g_modules.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        std::string n = me.szModule;   // ansi-ok: a module's name for the report (the system's code page)
        std::string l = n, path = me.szExePath;   // ansi-ok: only searched for "\mml\live\"
        for (char &c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (char &c : path) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        CodeKind kind = l == "game.exe" || l == "ls3df.dll" ? kCodeGame
                      : l == "mafiamodloader.asi" || path.find("\\mml\\live\\") != std::string::npos ? kCodeOurs : kCodeOther;
        uintptr_t b = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        g_modules.push_back({b, b + me.modBaseSize, n, kind});
    }
    CloseHandle(snap);
    std::sort(g_modules.begin(), g_modules.end(), [](const Mod &a, const Mod &b) { return a.base < b.base; });
}
const Mod *ModuleAt(uintptr_t a) {   // (by base: every value of a stack is asked)
    auto it = std::upper_bound(g_modules.begin(), g_modules.end(), a, [](uintptr_t x, const Mod &m) { return x < m.base; });
    if (it == g_modules.begin()) return nullptr;
    --it;
    return a < it->end ? &*it : nullptr;
}
// Whose code an address is, by the modules read before the run (the stack's reader: stack.cpp)
CodeKind ProfKind(uintptr_t a, void *) {
    const Mod *m = ModuleAt(a);
    return !m || a - m->base <= 0x1000 ? kCodeNone : m->kind;   // (the first page: the module's headers)
}

// The top of the stack copied while the thread is stopped (16 KB at most, to its base), read after it goes on.
constexpr size_t kStackCopy = 16384 / 4;
size_t CopyStack(uint32_t esp, uintptr_t base, uint32_t *out) {
    if (!esp || esp >= base) return 0;
    size_t n = std::min<size_t>(kStackCopy, (base - esp) / 4);
    __try {
        memcpy(out, reinterpret_cast<const void *>(static_cast<uintptr_t>(esp)), n * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

// The chain of a sample: the calls on its copied stack (StackCalls: walked by the frame pointers from the system's,
// the driver's or our code to the first frame of the game's, then every return address into the game's or our code),
// each with its call's target: pairs in g_chain. MCP's `profile` keeps a return address only where its call leads to
// the frame below it (a stale one, left in a frame's unwritten locals by a call that returned, is not).
constexpr int kChainMax = 48;
void Analyze(Sample &s, uint32_t esp, uint32_t ebp, const uint32_t *stack, size_t n) {
    const Mod *at = ModuleAt(s.eip);
    const bool out = !at || at->kind == kCodeOther;   // in a system or driver module
    s.chainAt = static_cast<uint32_t>(g_chain.size());
    StackCall calls[kChainMax];
    const StackView v = {s.eip, esp, ebp, stack, n};
    const int k = StackCalls(v, ProfKind, nullptr, false, calls, kChainMax);
    bool gameSeen = false;
    for (int i = 0; i < k; i++) {
        const uint32_t r = calls[i].ret;
        g_chain.push_back(r);
        g_chain.push_back(calls[i].target);
        if (ModuleAt(r)->kind == kCodeGame) {
            if (!gameSeen && out) s.caller = r;
            gameSeen = true;
        } else if (!gameSeen && !s.host && out) {
            s.host = r;
        }
    }
    s.chainN = static_cast<uint16_t>(k);
}

void Run(HANDLE target, uintptr_t stackBase, double seconds, double intervalMs) {
    // a timer of high resolution between samples (Windows 10 1803 and later; measured: 1 ms is its least), a wait on
    // the clock for a shorter interval (a core kept busy) or without the timer
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    std::vector<uint32_t> stack(kStackCopy);
    double t0 = NowMs(), stoppedMs = 0;
    while (NowMs() - t0 < seconds * 1000 && g_samples.size() < g_samples.capacity()) {
        double s0 = NowMs();
        if (SuspendThread(target) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            g_profHadSample = true;
            bool got = GetThreadContext(target, &c) != 0;
            size_t n = got ? CopyStack(c.Esp, stackBase, stack.data()) : 0;
            const int mod = g_curMod.load(), phase = g_phase.load();
            ResumeThread(target);
            stoppedMs += NowMs() - s0;
            if (got) {
                Sample s{c.Eip, 0, 0, 0, 0, static_cast<int16_t>(mod), static_cast<uint8_t>(phase)};
                Analyze(s, c.Esp, c.Ebp, stack.data(), n);
                g_samples.push_back(s);
            }
        }
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(intervalMs * 10000);
        if (intervalMs >= 1 && timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
        else for (double w0 = NowMs(); NowMs() - w0 < intervalMs;) YieldProcessor();
    }
    double took = (NowMs() - t0) / 1000;
    if (timer) CloseHandle(timer);
    CloseHandle(target);
    // the report (this thread: the game thread is not needed)
    auto at = [](uint32_t a) {
        const Mod *m = ModuleAt(a);
        char b[160];
        if (m) snprintf(b, sizeof b, "%s+0x%X", m->name.c_str(), static_cast<unsigned>(a - m->base));
        else snprintf(b, sizeof b, "0x%08X", a);
        return std::string(b);
    };
    size_t n = g_samples.size();
    auto pct = [&](size_t k) { return std::round(1000.0 * k / std::max<size_t>(n, 1)) / 10; };
    std::map<std::string, size_t> byModule, byMod, byCaller, byHost;
    std::map<std::pair<uint32_t, int>, size_t> spots;
    std::map<std::vector<uint32_t>, size_t> chains;   // [where, the chain's addresses...] as indices: samples
    std::map<uint32_t, uint32_t> index;              // an address: its place in `addresses`
    json addresses = json::array();
    auto indexOf = [&](uint32_t a) {
        auto it = index.find(a);
        if (it != index.end()) return it->second;
        uint32_t i = static_cast<uint32_t>(addresses.size());
        index[a] = i;
        addresses.push_back(at(a));
        return i;
    };
    size_t phases[kPhases] = {};
    std::vector<uint32_t> key;
    for (const Sample &s : g_samples) {
        const Mod *m = ModuleAt(s.eip);
        byModule[m ? m->name : "?"]++;
        byMod[s.mod >= 0 && s.mod < static_cast<int>(g_names.size()) ? g_names[s.mod] : "(no mod's call)"]++;
        if (s.caller) byCaller[at(s.caller)]++;
        if (s.host) byHost[at(s.host)]++;
        spots[{s.eip & ~0xFu, s.phase}]++;
        phases[s.phase < kPhases ? s.phase : 0]++;
        // the samples' stacks, alike ones together: MCP's `profile` makes them functions, each once a sample however
        // often it is on the stack (recursion)
        key.assign(1, indexOf(s.eip));
        for (uint32_t i = 0; i < s.chainN; i++) {
            const uint32_t target = g_chain[s.chainAt + 2 * i + 1];
            key.push_back(indexOf(g_chain[s.chainAt + 2 * i]));
            key.push_back(target == kCallWalked ? kCallWalked : target ? indexOf(target) : 0xFFFFFFFFu);
        }
        chains[key]++;
    }
    auto sorted = [&](const std::map<std::string, size_t> &m, size_t limit) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto &e : m) v.push_back({e.second, e.first});
        std::sort(v.rbegin(), v.rend());
        json out = json::array();
        for (size_t i = 0; i < v.size() && i < limit; i++) out.push_back({{"name", v[i].second}, {"percent", pct(v[i].first)}});
        return out;
    };
    json chainList = json::array();
    for (auto &c : chains) {
        json row = json::array({c.second});
        for (uint32_t x : c.first) row.push_back(x);
        chainList.push_back(row);
    }
    json ph = json::object();
    for (int p = 0; p < kPhases; p++) ph[kPhaseNames[p]] = pct(phases[p]);
    std::vector<std::pair<size_t, std::pair<uint32_t, int>>> sv;
    for (auto &e : spots) sv.push_back({e.second, e.first});
    std::sort(sv.rbegin(), sv.rend());
    json top = json::array();
    for (size_t i = 0; i < sv.size() && i < 40; i++)
        top.push_back({{"at", at(sv[i].second.first)}, {"percent", pct(sv[i].first)}, {"phase", kPhaseNames[sv[i].second.second < kPhases ? sv[i].second.second : 0]}});
    json r = {{"id", g_sampleId}, {"done", true}, {"samples", n}, {"seconds", std::round(took * 100) / 100},
              {"interval_ms", n ? std::round(took * 1000.0 / n * 1000) / 1000 : 0.0},
              {"stopped_percent", std::round(stoppedMs / (took * 10.0) * 100) / 100},
              {"phases", ph}, {"by_module", sorted(byModule, 20)}, {"by_mod", sorted(byMod, 20)},
              {"spots", top}, {"callers", sorted(byCaller, 25)}, {"host_callers", sorted(byHost, 25)},
              {"addresses", addresses}, {"chains", chainList},
              {"note", "callers: for samples in a module not the game's (Direct3D, the driver, the system), the first return "
                       "address into Game.exe or LS3DF.dll on the stack; host_callers: the loader's or a mod's code above it "
                       "(the host's own calls into Direct3D); chains: [samples, where the thread was, then for each return address "
                       "into the game's and our code on its stack, top first: it and its direct call's target (4294967295: "
                       "an indirect call; 4294967294: found by the frame pointers, certain)] (`addresses`' indices): MCP's `profile` makes them functions, self and total; "
                       "stopped_percent: the game thread's time the sampler held it"}};
    {
        std::lock_guard<std::mutex> lk(g_reportMx);
        g_report = r;
    }
    g_samples.clear();
    g_chain.clear();
    g_chain.shrink_to_fit();
    g_sampling = false;
}

} // namespace

// ---- the host's calls into mods (mods.cpp and the rest) and its own parts
void ProfEnter(const MmlMod *mod, const char *site) {
    if (g_depth < 16) {
        g_stack[g_depth] = g_curMod.load();
        g_curMod = mod ? NameIndex(ModIdOf(mod)) : g_curMod.load();
    }
    g_depth++;
    (void)site;
}
void ProfLeave(const MmlMod *mod, const char *site, double ms) {
    g_depth--;
    if (g_depth < 16 && g_depth >= 0) g_curMod = g_stack[g_depth];
    Stat &s = g_stats[{ModIdOf(mod), site}];
    if (s.site.empty()) { s.mod = ModIdOf(mod); s.site = site; }
    s.frameMs += ms;
    s.frameCalls++;
}
const char *ProfEventSite(int type) { return EventName(type); }
double ProfNow() { return NowMs(); }

void ProfPhase(int phase) {   // 0 restores the phase before the matching call with a phase
    if (phase) {
        if (g_phaseDepth < 16) g_phaseStack[g_phaseDepth] = g_phase.load();
        g_phaseDepth++;
        g_phase = phase;
    } else if (g_phaseDepth > 0) {
        g_phaseDepth--;
        g_phase = g_phaseDepth < 16 ? g_phaseStack[g_phaseDepth] : 0;
    }
}

bool g_profHotOn;
uint64_t g_profHotTicks[kHotCount];
uint32_t g_profHotCalls[kHotCount];
int g_profVisual = -1;

// ---- the visuals' m_AddFGroups timed by class (game_build.h addObject..addSinglemesh): hooked the first time the
// counters are on, and left (with the counters off a call costs the detour's jump and one test)
namespace {
using AddFn = void(__fastcall *)(void *visual, void *edx);
AddFn g_addOrig[5];
bool g_addHooked;
template <int K> void __fastcall AddHooked(void *visual, void *edx) {
    const int was = g_profVisual;
    g_profVisual = K;
    {
        ProfHotScope t(kHotVisual + K);
        g_addOrig[K](visual, edx);
    }
    g_profVisual = was;
}
void HookVisuals() {
    if (g_addHooked || !game::g_ls3d) return;
    g_addHooked = true;
    const uintptr_t ls3d = ModuleBase("LS3DF.dll");
    const game::Ls3dBuild &l = game::Ls3d();
    const uintptr_t at[5] = {l.addObject, l.addLitObject, l.addBillboard, l.addMorph, l.addSinglemesh};
    void *detours[5] = {reinterpret_cast<void *>(&AddHooked<0>), reinterpret_cast<void *>(&AddHooked<1>), reinterpret_cast<void *>(&AddHooked<2>),
                        reinterpret_cast<void *>(&AddHooked<3>), reinterpret_cast<void *>(&AddHooked<4>)};
    int n = 0;
    std::string err;
    for (int k = 0; k < 5; k++)
        if (ls3d && at[k] && HookAdd(nullptr, ls3d + at[k], detours[k], reinterpret_cast<void **>(&g_addOrig[k]), &err)) n++;
    HLOG("prof: %d of the 5 visual classes' m_AddFGroups timed (prof.frames' hot_ms visuals.*)%s%s", n, err.empty() ? "" : ": ", err.c_str());
}
} // namespace

uint32_t ProfFrameSeq() { return g_seq; }
double ProfTscPerMs() { return g_tscPerMs; }
void ProfGpuOf(uint32_t seq, float ms) {
    if (FrameRec *r = RecOf(seq)) r->gpuMs = ms;
}

namespace {
void FrameTimes(double now);

// The frame's record closed (the game's Present has returned), the next one begun.
void CloseFrame(double now, uint64_t tc) {
    TscTrack();
    if (!g_frameRing) g_frameRing = new FrameRec[kFrames]();
    if (g_frameStartMs > 0) {
        FrameRec &r = g_frameRing[g_seq % kFrames];
        r = FrameRec{};
        r.seq = g_seq;
        const double perMs = g_tscPerMs > 0 ? g_tscPerMs : 0;
        auto ms = [&](uint64_t ticks) { return perMs ? static_cast<float>(ticks / perMs) : NAN; };
        r.wallMs = static_cast<float>(now - g_frameStartMs);
        r.presentMs = g_presentMs;
        r.presentCpuMs = g_presentCpuMs;
        r.cpuMs = ms(tc - g_frameThreadC);
        r.gpuMs = NAN;
        if (g_settleLeft > 0) { r.flags |= kFlagSettling; g_settleLeft--; }
        else r.label = g_label;
        if (g_presentHr != 0) r.flags |= kFlagPresentNotOk;
        if (g_profHadSample.exchange(false)) r.flags |= kFlagSampled;
        if (D3dStatsOn()) {
            r.flags |= kFlagCounted;
            D3dStatsTake(&r.d3d);
        }
        for (int i = 0; i < kHotCount; i++) {
            r.hotMs[i] = g_profHotOn ? ms(g_profHotTicks[i]) : 0;
            g_profHotTicks[i] = 0;
        }
        g_seq++;
    }
    g_frameStartMs = now;
    g_frameThreadC = tc;
    g_presentMs = g_presentCpuMs = 0;
    g_presentHr = 0;
}
} // namespace

void ProfPresentBegin() {
    g_presentStartMs = NowMs();
    g_presentStartC = ThreadCycles();
    D3dStatsFrameEnd(g_seq);
}
void ProfPresentEnd(long hr) {
    double now = NowMs();
    g_presentMs += static_cast<float>(now - g_presentStartMs);
    const uint64_t tc = ThreadCycles();
    if (g_tscPerMs > 0) g_presentCpuMs += static_cast<float>((tc - g_presentStartC) / g_tscPerMs);
    if (hr != 0) g_presentHr = hr;
    FrameTimes(now);
    CloseFrame(now, tc);
    D3dStatsFrameStart();
}

namespace {
// the frames' times (prof.state's `frame`, the hitches, the mods' sites' averages)
void FrameTimes(double now) {
    if (g_lastPresent > 0) {
        float ms = static_cast<float>(now - g_lastPresent);
        g_ring[g_ringAt] = ms;
        g_ringAt = (g_ringAt + 1) % kRing;
        if (g_ringN < kRing) g_ringN++;
        g_frames++;
        // a hitch: much longer than the median of the frames before it
        if (g_ringN >= 60) {
            std::vector<float> v(g_ring, g_ring + g_ringN);
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            float med = v[v.size() / 2];
            if (ms > std::max(med * 2.5f, med + 8.0f)) {
                std::vector<const Stat *> parts;
                for (auto &kv : g_stats) if (kv.second.frameMs > 0.05) parts.push_back(&kv.second);
                std::sort(parts.begin(), parts.end(), [](const Stat *a, const Stat *b) { return a->frameMs > b->frameMs; });
                json pj = json::array();
                for (size_t i = 0; i < parts.size() && i < 8; i++)
                    pj.push_back({{"mod", parts[i]->mod}, {"site", parts[i]->site}, {"ms", std::round(parts[i]->frameMs * 100) / 100}});
                g_hitches.push_back({(now - g_resetAt) / 1000, ms, med, pj});
                if (g_hitches.size() > 10) g_hitches.erase(g_hitches.begin());
            }
        }
        for (auto &kv : g_stats) {
            Stat &s = kv.second;
            s.ema += (s.frameMs - s.ema) * (1.0 / 60);
            if (s.frameCalls) {
                s.totalMs += s.frameMs;
                s.maxMs = std::max(s.maxMs, s.frameMs);
                s.calls += s.frameCalls;
                s.framesWith++;
            }
            s.frameMs = 0;
            s.frameCalls = 0;
        }
    }
    g_lastPresent = now;
}
} // namespace

bool ProfFrameStats(const MmlMod *mod, MmlFrameStats *out) {
    if (!out || out->size < sizeof(uint32_t)) return false;
    MmlFrameStats s{};
    s.size = sizeof s;
    if (g_ringN) {
        std::vector<float> v(g_ring, g_ring + g_ringN);
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (float x : v) sum += x;
        s.avg_ms = static_cast<float>(sum / v.size());
        s.median_ms = v[v.size() / 2];
        s.p95_ms = v[std::min(v.size() - 1, v.size() * 95 / 100)];
        s.last_ms = g_ring[(g_ringAt + kRing - 1) % kRing];
    }
    std::string id = ModIdOf(mod);
    for (auto &kv : g_stats) {
        const Stat &st = kv.second;
        if (st.mod == id) s.mod_ms += static_cast<float>(st.ema);
        else if (st.mod == "host" && st.site == "scene views") s.views_ms = static_cast<float>(st.ema);
        else if (st.mod == "host" && st.site == "graphics bridge") s.bridge_ms = static_cast<float>(st.ema);
    }
    s.frames = g_frames;
    memcpy(out, &s, std::min<size_t>(out->size, sizeof s));
    out->size = std::min<uint32_t>(out->size, sizeof s);
    return true;
}

bool ProfMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.rfind("prof.", 0) != 0) return false;
    *ok = true;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto r2 = [](double v) { return std::round(v * 1000) / 1000; };
    if (method == "prof.reset") {
        g_stats.clear();
        g_frames = 0;
        g_ringN = g_ringAt = 0;
        g_hitches.clear();
        g_resetAt = NowMs();
        *reply = "{\"reset\":true}";
        return true;
    }
    if (method == "prof.state") {
        json frame = nullptr;
        if (g_ringN) {
            std::vector<float> v(g_ring, g_ring + g_ringN);
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (float x : v) sum += x;
            float last = g_ring[(g_ringAt + kRing - 1) % kRing];
            frame = {{"frames", g_ringN}, {"avg_ms", r2(sum / v.size())}, {"median_ms", r2(v[v.size() / 2])},
                     {"p95_ms", r2(v[std::min(v.size() - 1, v.size() * 95 / 100)])}, {"max_ms", r2(v.back())}, {"last_ms", r2(last)},
                     {"fps", r2(1000.0 * v.size() / sum)}};
        }
        // by mod: its sites, a frame's average (over every frame since the reset) and the worst frame
        std::map<std::string, json> mods;
        std::map<std::string, double> modAvg;
        for (auto &kv : g_stats) {
            const Stat &s = kv.second;
            double avg = g_frames ? s.totalMs / g_frames : 0;
            json site = {{"site", s.site}, {"ms_per_frame", r2(avg)}, {"worst_frame_ms", r2(s.maxMs)},
                         {"calls_per_frame", g_frames ? std::round(10.0 * s.calls / g_frames) / 10 : 0}};
            if (!mods.count(s.mod)) mods[s.mod] = json::array();
            mods[s.mod].push_back(site);
            if (s.mod != "host") modAvg[s.mod] += avg;
        }
        json out = json::array();
        for (auto &kv : mods) {
            std::sort(kv.second.begin(), kv.second.end(), [](const json &a, const json &b) { return a["ms_per_frame"] > b["ms_per_frame"]; });
            json m = {{"mod", kv.first}, {"sites", kv.second}};
            if (kv.first != "host") m["ms_per_frame"] = r2(modAvg[kv.first]);
            else m["note"] = "the host's own parts; the mods' calls inside them (views' functions, passes) are in both";
            out.push_back(m);
        }
        std::sort(out.begin(), out.end(), [](const json &a, const json &b) { return a.value("ms_per_frame", 1e9) > b.value("ms_per_frame", 1e9); });
        json hitches = json::array();
        for (const Hitch &h : g_hitches)
            hitches.push_back({{"at_s", r2(h.atS)}, {"ms", r2(h.ms)}, {"median_ms", r2(h.median)}, {"parts", h.parts}});
        DWORD fgPid = 0;
        HWND fg = GetForegroundWindow();
        const bool inFront = fg && GetWindowThreadProcessId(fg, &fgPid) && fgPid == GetCurrentProcessId();
        json res = {{"frame", frame}, {"since_reset_s", r2((NowMs() - g_resetAt) / 1000)}, {"mods", out}, {"hitches", hitches},
                    {"sampling", g_sampling.load()}, {"seq", g_seq}, {"label", g_labels[g_label]}, {"settling", g_settleLeft},
                    {"counters", D3dStatsOn()}, {"gpu", D3dStatsWhyGpu().empty() ? json("measured") : json(D3dStatsWhyGpu())},
                    {"tsc_per_ms", r2(g_tscPerMs)}, {"in_front", inFront}};
        *reply = res.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "prof.mark") {
        // the frames from now on carry this label, after `settle` frames let pass (a switch's own frames: shaders
        // made, caches filled)
        std::string label = p.value("label", std::string());
        int settle = std::min(std::max(p.value("settle", 30), 0), 1000);
        uint16_t idx = 0;
        if (!label.empty()) {
            auto it = std::find(g_labels.begin(), g_labels.end(), label);
            if (it == g_labels.end()) {
                if (g_labels.size() >= 1000) { *ok = false; *reply = "prof.mark: 1000 labels at most"; return true; }
                g_labels.push_back(label);
                it = g_labels.end() - 1;
            }
            idx = static_cast<uint16_t>(it - g_labels.begin());
        }
        g_label = idx;
        g_settleLeft = idx ? settle : 0;
        *reply = json({{"label", label}, {"from_seq", g_seq + static_cast<uint32_t>(g_settleLeft)}, {"seq", g_seq}})
                     .dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "prof.counters") {
        bool on = p.value("on", true);
        bool gpu = p.value("gpu", on);
        std::string why;
        bool done = D3dStatsSet(on, gpu, &why);
        if (on && done) HookVisuals();
        g_profHotOn = on && done;
        for (int i = 0; i < kHotCount; i++) g_profHotTicks[i] = 0, g_profHotCalls[i] = 0;
        json r = {{"on", D3dStatsOn()}, {"gpu", D3dStatsWhyGpu().empty() ? json("measured") : json(D3dStatsWhyGpu())}};
        if (!why.empty()) r["why"] = why;
        if (on && !done) { *ok = false; *reply = "prof.counters: " + why; return true; }
        *reply = r.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "prof.frames") {
        // the frames after `since` (a seq), columns of numbers
        uint32_t since = p.value("since", 0u);
        size_t limit = std::min<size_t>(std::max(p.value("limit", 1000), 1), kFrames);
        uint32_t from = std::max<uint32_t>(since + 1, g_seq > kFrames ? g_seq - static_cast<uint32_t>(kFrames) + 1 : 1);
        std::vector<const FrameRec *> rs;
        for (uint32_t s = from; s < g_seq && rs.size() < limit; s++)
            if (const FrameRec *r = RecOf(s)) rs.push_back(r);
        auto num = [](float v) { return std::isnan(v) ? json(nullptr) : json(std::round(v * 1000.0) / 1000.0); };
        auto col = [&](auto get) { json a = json::array(); for (const FrameRec *r : rs) a.push_back(get(r)); return a; };
        json f = {{"seq", col([](const FrameRec *r) { return r->seq; })},
                  {"label", col([](const FrameRec *r) { return r->label; })},
                  {"flags", col([](const FrameRec *r) { return r->flags; })},
                  {"wall_ms", col([&](const FrameRec *r) { return num(r->wallMs); })},
                  {"present_ms", col([&](const FrameRec *r) { return num(r->presentMs); })},
                  {"present_cpu_ms", col([&](const FrameRec *r) { return num(r->presentCpuMs); })},
                  {"cpu_ms", col([&](const FrameRec *r) { return num(r->cpuMs); })},
                  {"gpu_ms", col([&](const FrameRec *r) { return num(r->gpuMs); })}};
        bool counted = std::any_of(rs.begin(), rs.end(), [](const FrameRec *r) { return r->flags & kFlagCounted; });
        if (counted) {
            static const char *hot[kHotCount] = {"skin.update_lod", "skin.update_lod.game", "skin.copy", "skin.copy.game",
                                                 "skin.lighting", "skin.lighting.game",
                                                 "visuals.object", "visuals.lit_object", "visuals.billboard", "visuals.morph", "visuals.singlemesh",
                                                 "lighting.object", "lighting.lit_object", "lighting.billboard", "lighting.morph", "lighting.singlemesh"};
            json h = json::object();
            for (int i = 0; i < kHotCount; i++)
                if (std::any_of(rs.begin(), rs.end(), [&](const FrameRec *r) { return r->hotMs[i] > 0; }))
                    h[hot[i]] = col([&](const FrameRec *r) { return num(r->hotMs[i]); });
            f["hot_ms"] = h;
            const double perMs = g_tscPerMs > 0 ? g_tscPerMs : 1;
            auto tms = [&](uint64_t t) { return std::round(t / perMs * 1000.0) / 1000.0; };
            json d = json::object();
            const char *who[kWhoCount] = {"game", "host"};
            for (int k = 0; k < kKindCount; k++)
                for (int w = 0; w < kWhoCount; w++) {
                    if (!std::any_of(rs.begin(), rs.end(), [&](const FrameRec *r) { return r->d3d.calls[k][w] > 0; })) continue;
                    d[D3dKindName(k)][who[w]] = {{"calls", col([&](const FrameRec *r) { return r->d3d.calls[k][w]; })},
                                                 {"ms", col([&](const FrameRec *r) { return tms(r->d3d.ticks[k][w]); })}};
                }
            for (int w = 0; w < kWhoCount; w++) {
                d["primitives"][who[w]] = col([&](const FrameRec *r) { return r->d3d.prims[w]; });
                d["driver_ms"][who[w]] = col([&](const FrameRec *r) { return tms(r->d3d.driverTicks[w]); });
            }
            d["driver_draws"] = {{"fixed", col([](const FrameRec *r) { return r->d3d.driverDraws[0]; })},
                                 {"shaders", col([](const FrameRec *r) { return r->d3d.driverDraws[1]; })}};
            d["switches"] = col([](const FrameRec *r) { return r->d3d.switches; });
            f["d3d"] = d;
        }
        json r = {{"frames", f}, {"count", rs.size()}, {"next", rs.empty() ? since : rs.back()->seq},
                  {"now", g_seq}, {"labels", g_labels}, {"tsc_per_ms", g_tscPerMs}};
        *reply = r.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "prof.sample") {
        int id = p.value("id", 0);
        if (id) {
            std::lock_guard<std::mutex> lk(g_reportMx);
            if (!g_report.is_null() && g_report.value("id", 0) == id) *reply = g_report.dump(-1, ' ', false, json::error_handler_t::replace);
            else *reply = json({{"id", id}, {"done", false}, {"running", g_sampling.load()}}).dump(-1, ' ', false, json::error_handler_t::replace);
            return true;
        }
        if (g_sampling) { *ok = false; *reply = "prof.sample: a run is going on (prof.sample {id})"; return true; }
        double seconds = std::min(std::max(p.value("seconds", 3.0), 0.5), 30.0);
        double interval = std::min(std::max(p.value("interval_ms", 1.0), 0.25), 10.0);
        HANDLE me = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
        if (!me) { *ok = false; *reply = "prof.sample: the game thread could not be opened"; return true; }
        ReadModules();
        g_samples.clear();
        g_samples.reserve(static_cast<size_t>(seconds * 1100 / interval) + 16);
        g_chain.reserve(static_cast<size_t>(seconds * 1100 / interval) * 32);
        g_sampleId = g_nextSampleId++;
        g_sampleSeconds = seconds;
        g_sampling = true;
        const uintptr_t stackBase = __readfsdword(4);   // (this is the game thread: its stack's base, NT_TIB)
        std::thread(Run, me, stackBase, seconds, interval).detach();   // (it ends by itself after `seconds`; nothing waits for it)
        *reply = json({{"id", g_sampleId}, {"seconds", seconds}, {"next", "prof.sample {id} once it ran"}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    *ok = false;
    *reply = "unknown prof method (prof.state, prof.reset, prof.sample, prof.mark, prof.frames, prof.counters)";
    return true;
}


} // namespace host
