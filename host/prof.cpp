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
                                  "CONFIG_CHANGED", "FILE_CHANGED", "RENDER_BEGIN", "RENDER_WORLD", "RENDER_END", "PRESENT", "VIEW_WORLD"};
    return type >= 0 && type < static_cast<int>(sizeof names / sizeof names[0]) ? names[type] : "event";
}

// ---- the sampler
const char *kPhaseNames[] = {"game", "frame drawing", "scene views", "graphics bridge"};
constexpr int kPhases = 4;
struct Sample { uint32_t eip, caller; int16_t mod; uint8_t phase; };
std::vector<Sample> g_samples;
std::atomic<bool> g_sampling{false};
int g_sampleId, g_nextSampleId = 1;
double g_sampleSeconds;
std::mutex g_reportMx;
json g_report;                  // the last finished run's (with its id)

struct Mod { uintptr_t base, end; std::string name; bool game; };
std::vector<Mod> g_modules;     // read before a run: no module is looked up while the game thread is stopped

void ReadModules() {
    g_modules.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        std::string n = me.szModule;
        std::string l = n;
        for (char &c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        bool game = l == "game.exe" || l == "ls3df.dll";
        uintptr_t b = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        g_modules.push_back({b, b + me.modBaseSize, n, game});
    }
    CloseHandle(snap);
}
const Mod *ModuleAt(uintptr_t a) {
    for (const Mod &m : g_modules) if (a >= m.base && a < m.end) return &m;
    return nullptr;
}

// A return address: inside a game module, right after a call instruction.
bool AfterCall(uintptr_t a) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(a);
    return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) ||
           (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10);
}
uint32_t CallerOf(const CONTEXT &c) {
    // the first return address into Game.exe / LS3DF.dll on the stack, within 2 KB of its top
    const uint32_t *sp = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(c.Esp));
    __try {
        for (int i = 0; i < 512; i++) {
            uint32_t v = sp[i];
            const Mod *m = ModuleAt(v);
            if (m && m->game && v - m->base > 0x1000 && AfterCall(v)) return v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

void Run(HANDLE target, double seconds) {
    double t0 = NowMs();
    while (NowMs() - t0 < seconds * 1000 && g_samples.size() < g_samples.capacity()) {
        if (SuspendThread(target) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(target, &c)) {
                const Mod *m = ModuleAt(c.Eip);
                uint32_t caller = (m && m->game) ? 0 : CallerOf(c);
                g_samples.push_back({c.Eip, caller, static_cast<int16_t>(g_curMod.load()), static_cast<uint8_t>(g_phase.load())});
            }
            ResumeThread(target);
        }
        double w0 = NowMs();
        while (NowMs() - w0 < 0.5) YieldProcessor();
    }
    double took = (NowMs() - t0) / 1000;
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
    std::map<std::string, size_t> byModule, byMod, byCaller;
    std::map<std::pair<uint32_t, int>, size_t> spots;
    size_t phases[kPhases] = {};
    for (const Sample &s : g_samples) {
        const Mod *m = ModuleAt(s.eip);
        byModule[m ? m->name : "?"]++;
        byMod[s.mod >= 0 && s.mod < static_cast<int>(g_names.size()) ? g_names[s.mod] : "(no mod's call)"]++;
        if (s.caller) byCaller[at(s.caller)]++;
        spots[{s.eip & ~0xFu, s.phase}]++;
        phases[s.phase < kPhases ? s.phase : 0]++;
    }
    auto sorted = [&](const std::map<std::string, size_t> &m, size_t limit) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto &e : m) v.push_back({e.second, e.first});
        std::sort(v.rbegin(), v.rend());
        json out = json::array();
        for (size_t i = 0; i < v.size() && i < limit; i++) out.push_back({{"name", v[i].second}, {"percent", pct(v[i].first)}});
        return out;
    };
    json ph = json::object();
    for (int p = 0; p < kPhases; p++) ph[kPhaseNames[p]] = pct(phases[p]);
    std::vector<std::pair<size_t, std::pair<uint32_t, int>>> sv;
    for (auto &e : spots) sv.push_back({e.second, e.first});
    std::sort(sv.rbegin(), sv.rend());
    json top = json::array();
    for (size_t i = 0; i < sv.size() && i < 40; i++)
        top.push_back({{"at", at(sv[i].second.first)}, {"percent", pct(sv[i].first)}, {"phase", kPhaseNames[sv[i].second.second < kPhases ? sv[i].second.second : 0]}});
    json r = {{"id", g_sampleId}, {"done", true}, {"samples", n}, {"seconds", std::round(took * 100) / 100},
              {"phases", ph}, {"by_module", sorted(byModule, 20)}, {"by_mod", sorted(byMod, 20)},
              {"spots", top}, {"callers", sorted(byCaller, 25)},
              {"note", "callers: for samples in a module not the game's (Direct3D, the driver, the system), the first return "
                       "address into Game.exe or LS3DF.dll on the stack"}};
    {
        std::lock_guard<std::mutex> lk(g_reportMx);
        g_report = r;
    }
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

// (render.cpp) a frame is complete: Present.
void ProfFrame() {
    double now = NowMs();
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
        json res = {{"frame", frame}, {"since_reset_s", r2((NowMs() - g_resetAt) / 1000)}, {"mods", out}, {"hitches", hitches},
                    {"sampling", g_sampling.load()}};
        *reply = res.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "prof.sample") {
        int id = p.value("id", 0);
        if (id) {
            std::lock_guard<std::mutex> lk(g_reportMx);
            if (!g_report.is_null() && g_report.value("id", 0) == id) *reply = g_report.dump(-1, ' ', false, json::error_handler_t::replace);
            else *reply = json({{"id", id}, {"done", false}, {"running", g_sampling.load()}}).dump();
            return true;
        }
        if (g_sampling) { *ok = false; *reply = "prof.sample: a run is going on (prof.sample {id})"; return true; }
        double seconds = std::min(std::max(p.value("seconds", 3.0), 0.5), 30.0);
        HANDLE me = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
        if (!me) { *ok = false; *reply = "prof.sample: the game thread could not be opened"; return true; }
        ReadModules();
        g_samples.clear();
        g_samples.reserve(static_cast<size_t>(seconds * 2200) + 16);
        g_sampleId = g_nextSampleId++;
        g_sampleSeconds = seconds;
        g_sampling = true;
        std::thread(Run, me, seconds).detach();   // (it ends by itself after `seconds`; nothing waits for it)
        *reply = json({{"id", g_sampleId}, {"seconds", seconds}, {"next", "prof.sample {id} once it ran"}}).dump();
        return true;
    }
    *ok = false;
    *reply = "unknown prof method (prof.state, prof.reset, prof.sample)";
    return true;
}


} // namespace host
