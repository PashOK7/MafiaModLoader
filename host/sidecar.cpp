// The 64-bit sidecar, the loader's side (sidecar/main.cpp is the other; sdk/include/mml/side.h the mods' ABI;
// docs/MODDING.md "64-bit halves").
//
// A mod whose manifest names "entry64" has a 64-bit half. At the first such load the loader makes the shared memory
// sidecar/proto.h describes and starts <game>\mml\sidecar\mml-sidecar64.exe in a job object that ends it with the
// game; the messages wait in the ring until it reads them, so loads are sent at once. From then on:
//   - a half loaded, reloaded or unloaded with its 32-bit half (mods.cpp calls SidecarModLoad / SidecarModUnload);
//   - the events the halves subscribed to sent as they happen (every event kept for agents: EventsRecord; the
//     ticks: SidecarTick, SidecarFrame), never waiting for room (a full ring drops them, counted);
//   - a half's call to the bridge answered as an agent's (BridgeCall, on one of a few threads: a call that waits for
//     the game thread does not hold up the rest);
//   - a bridge call to a half's command ("<mod>.<name>") sent to the sidecar from the bridge's thread, the game's
//     frame not waited for;
//   - a half's shared textures kept by name (their handles in this process), for its 32-bit half's passes.
// When the sidecar ends without being asked (a crash), its halves are marked gone, SIDECAR_EXITED is told, and it is
// started again after 2 s (at most 3 times a minute) with the halves that were loaded.
//
// Bridge methods: sidecar.state (bridge thread), sidecar.restart (game thread).
#include "host.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include "json.hpp"
#include "../sidecar/channel.h"
#include "../sidecar/proto.h"
#include "../sidecar/ring.h"

using json = nlohmann::json;

struct MmlChannel {   // (mml.h's opaque channel)
    side::Channel ch;
    MmlMod *mod;
};

namespace host {
json BridgeCall(const std::string &method, const json &params, int timeoutMs);   // (bridge.cpp)

namespace {

std::string Dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::mutex g_mx;                 // everything below
HANDLE g_map, g_job, g_proc;
uint8_t *g_base;
side::RingEnd g_out, g_in;       // g_out: to the sidecar; g_in: from it
std::mutex g_outMx;
std::string g_state = "not started (no mod has a 64-bit half)";
DWORD g_pid, g_exitCode;
bool g_started, g_wanted, g_quitting;
std::deque<ULONGLONG> g_restarts;
uint32_t g_luid[2];
bool g_haveLuid;
uint64_t g_dropped, g_sent;

struct HalfState {
    std::string dir, entry, dataDir;
    unsigned generation = 0;
    std::string state = "loading", error, where;
    std::set<std::string> subs;
};
std::map<std::string, HalfState> g_halves;
std::map<std::string, std::string> g_commands;   // "<mod>.<name>" -> help
struct SharedTex { HANDLE handle; uint32_t w, h, format, generation; uint64_t stamp; };
uint64_t g_texStamp;
std::map<std::string, std::map<std::string, SharedTex>> g_textures;   // mod -> name -> texture
std::set<std::string> g_subTypes;   // the union of every half's subscriptions ("*": all)
std::atomic<bool> g_anySub{false}, g_tickSub{false}, g_frameSub{false};

// a command's answer, waited for by the bridge's thread
struct Wait { bool done = false, ok = false; std::string text; };
std::mutex g_waitMx;
std::condition_variable g_waitCv;
std::map<uint32_t, Wait *> g_waits;
std::atomic<uint32_t> g_seq{1};

// the half's calls, run by a few threads
std::mutex g_callMx;
std::condition_variable g_callCv;
std::deque<std::pair<uint32_t, std::string>> g_calls;
bool g_callThreads;

bool Send(uint32_t type, uint32_t aux, const json &j, uint32_t waitMs) {
    std::string s = j.is_null() ? std::string() : Dump(j);
    std::lock_guard<std::mutex> lk(g_outMx);
    if (!g_base) return false;
    return side::RingWrite(g_out, type, aux, s.data(), static_cast<uint32_t>(s.size()), waitMs);
}

void UpdateSubs() {   // (g_mx held)
    g_subTypes.clear();
    for (auto &kv : g_halves) g_subTypes.insert(kv.second.subs.begin(), kv.second.subs.end());
    g_anySub = !g_subTypes.empty();
    g_tickSub = g_subTypes.count("*") || g_subTypes.count("MISSION_TICK");
    g_frameSub = g_subTypes.count("*") || g_subTypes.count("FRAME");
}

void CallWorker() {
    for (;;) {
        std::pair<uint32_t, std::string> c;
        {
            std::unique_lock<std::mutex> lk(g_callMx);
            g_callCv.wait(lk, [] { return !g_calls.empty(); });
            c = std::move(g_calls.front());
            g_calls.pop_front();
        }
        json req = json::parse(c.second, nullptr, false);
        json reply;
        if (!req.is_object()) reply = {{"ok", false}, {"error", "a broken call"}};
        else {
            json params = req.contains("params") ? req["params"] : json::object();
            json r = BridgeCall(req.value("method", ""), params, req.value("timeout_ms", 5000));
            reply = r.contains("error") ? json{{"ok", false}, {"error", r["error"].value("message", std::string("error"))}}
                                        : json{{"ok", true}, {"result", r.contains("result") ? r["result"] : json()}};
        }
        Send(side::kCallReply, c.first, reply, 5000);
    }
}

void Handle(uint32_t type, uint32_t aux, const std::string &payload) {
    json j = payload.empty() ? json() : json::parse(payload, nullptr, false);
    if (j.is_discarded()) return;
    switch (type) {
    case side::kReady: {
        std::lock_guard<std::mutex> lk(g_mx);
        g_state = "running";
        HLOG("sidecar: mml-sidecar64 %u ready", j.value("pid", 0u));
        break;
    }
    case side::kLog: {
        int level = std::clamp(j.value("level", 1), 0, 3);
        std::string who = j.value("mod", std::string("sidecar"));
        std::string text = j.value("text", std::string());
        Log(static_cast<MmlLogLevel>(level), who == "sidecar" ? "sidecar" : (who + " (x64)").c_str(), "%s", text.c_str());
        break;
    }
    case side::kCall: {
        {
            std::lock_guard<std::mutex> lk(g_callMx);
            g_calls.emplace_back(aux, payload);
        }
        g_callCv.notify_one();
        break;
    }
    case side::kCommandReply: {
        std::lock_guard<std::mutex> lk(g_waitMx);
        auto it = g_waits.find(aux);
        if (it != g_waits.end()) {
            it->second->ok = j.value("ok", false);
            it->second->text = j.value("text", std::string());
            it->second->done = true;
            g_waitCv.notify_all();
        }
        break;
    }
    case side::kCommandAdd: {
        std::lock_guard<std::mutex> lk(g_mx);
        g_commands[j.value("mod", std::string()) + "." + j.value("name", std::string())] = j.value("help", std::string());
        break;
    }
    case side::kCommandRemove: {
        std::lock_guard<std::mutex> lk(g_mx);
        g_commands.erase(j.value("mod", std::string()) + "." + j.value("name", std::string()));
        break;
    }
    case side::kSubscribe: {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_halves.find(j.value("mod", std::string()));
        if (it == g_halves.end()) break;
        it->second.subs.clear();
        if (j.contains("types") && j["types"].is_array())
            for (auto &t : j["types"]) if (t.is_string()) it->second.subs.insert(t.get<std::string>());
        UpdateSubs();
        break;
    }
    case side::kModState: {
        std::string id = j.value("id", std::string()), state = j.value("state", std::string());
        {
            std::lock_guard<std::mutex> lk(g_mx);
            auto it = g_halves.find(id);
            if (it != g_halves.end() && j.value("generation", 0u) == it->second.generation) {
                it->second.state = state;
                it->second.error = j.value("error", std::string());
                it->second.where = j.value("where", std::string());
                if (state != "loaded") {
                    it->second.subs.clear();
                    UpdateSubs();
                }
            }
        }
        if (state == "failed") HWARN("mod %s: its 64-bit half did not load: %s", id.c_str(), j.value("error", std::string()).c_str());
        if (state == "faulted")
            EventsRecord("MOD_FAULTED", Dump({{"name", id}, {"half", "x64"}, {"where", j.value("where", std::string())},
                                              {"error", j.value("error", std::string())}}));
        break;
    }
    case side::kTexture: {
        std::lock_guard<std::mutex> lk(g_mx);
        auto &m = g_textures[j.value("mod", std::string())];
        std::string name = j.value("name", std::string());
        auto it = m.find(name);
        if (it != m.end() && it->second.handle) CloseHandle(it->second.handle);
        m[name] = SharedTex{reinterpret_cast<HANDLE>(static_cast<uintptr_t>(j.value("handle", 0ull))), j.value("width", 0u),
                            j.value("height", 0u), j.value("format", 0u), j.value("generation", 0u), ++g_texStamp};
        break;
    }
    case side::kTextureGone: {
        std::lock_guard<std::mutex> lk(g_mx);
        auto &m = g_textures[j.value("mod", std::string())];
        auto it = m.find(j.value("name", std::string()));
        if (it != m.end()) {
            if (it->second.handle) CloseHandle(it->second.handle);
            m.erase(it);
        }
        break;
    }
    default:
        break;
    }
}

DWORD WINAPI Reader(void *) {
    for (;;) {
        uint32_t type = 0, aux = 0;
        std::string payload;
        bool any = false;
        while (side::RingRead(g_in, &type, &aux, &payload)) {
            any = true;
            Handle(type, aux, payload);
        }
        if (!any) WaitForSingleObject(g_in.dataEvent, 100);
    }
}

bool MakeSection(std::string *err) {
    if (g_base) return true;
    wchar_t sec[64], sd[64], ss[64], hd[64], hs[64];
    side::Names(GetCurrentProcessId(), sec, sd, ss, hd, hs);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, side::kSectionBytes, sec);
    if (!g_map) { *err = "CreateFileMapping failed (" + std::to_string(GetLastError()) + ")"; return false; }
    g_base = static_cast<uint8_t *>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, side::kSectionBytes));
    if (!g_base) { *err = "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")"; return false; }
    auto *s = reinterpret_cast<side::Section *>(g_base);
    memset(s, 0, sizeof *s);
    s->version = side::kVersion;
    s->gamePid = GetCurrentProcessId();
    g_out.ring = reinterpret_cast<side::Ring *>(g_base + side::kOffToSide);
    g_out.data = g_base + side::kOffToSideData;
    g_in.ring = reinterpret_cast<side::Ring *>(g_base + side::kOffToHost);
    g_in.data = g_base + side::kOffToHostData;
    memset(g_out.ring, 0, sizeof(side::Ring));
    memset(g_in.ring, 0, sizeof(side::Ring));
    g_out.ring->size = g_in.ring->size = side::kRingBytes;
    g_out.dataEvent = CreateEventW(nullptr, FALSE, FALSE, sd);
    g_out.spaceEvent = CreateEventW(nullptr, FALSE, FALSE, ss);
    g_in.dataEvent = CreateEventW(nullptr, FALSE, FALSE, hd);
    g_in.spaceEvent = CreateEventW(nullptr, FALSE, FALSE, hs);
    MemoryBarrier();
    s->magic = side::kMagic;
    CreateThread(nullptr, 0, Reader, nullptr, 0, nullptr);
    if (!g_callThreads) {
        g_callThreads = true;
        for (int i = 0; i < 4; i++) std::thread(CallWorker).detach();
    }
    return true;
}

json Hello() {
    json j = {{"game_pid", GetCurrentProcessId()}, {"game_dir", GameDir()}, {"mml_dir", MmlDir()}, {"build", GameBuild()}};
    j["luid"] = g_haveLuid ? json::array({g_luid[0], g_luid[1]}) : json();
    return j;
}

json LoadMsg(const std::string &id, const HalfState &h) {
    return {{"id", id}, {"dir", h.dir}, {"entry", h.entry}, {"data_dir", h.dataDir}, {"generation", h.generation}};
}

DWORD WINAPI Watcher(void *proc) {
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    bool again = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (proc != g_proc) return 0;
        g_exitCode = code;
        CloseHandle(g_proc);
        g_proc = nullptr;
        g_started = false;
        if (g_quitting) return 0;
        for (auto &kv : g_halves) { kv.second.state = "gone (the sidecar ended)"; kv.second.subs.clear(); }
        UpdateSubs();
        g_commands.clear();
        for (auto &m : g_textures) for (auto &t : m.second) if (t.second.handle) CloseHandle(t.second.handle);
        g_textures.clear();
        ULONGLONG now = GetTickCount64();
        while (!g_restarts.empty() && now - g_restarts.front() > 60000) g_restarts.pop_front();
        again = g_wanted && g_restarts.size() < 3;
        char b[200];
        snprintf(b, sizeof b, "ended with code 0x%08lX%s", static_cast<unsigned long>(code),
                 again ? "; started again in 2 s" : g_wanted ? "; not started again (3 times in a minute: sidecar.restart)" : "");
        g_state = b;
    }
    Log(MML_LOG_ERROR, "host", "sidecar: mml-sidecar64 %s", g_state.c_str());
    EventsRecord("SIDECAR_EXITED", Dump({{"code", code}, {"restart", again}}));
    if (again) {
        Sleep(2000);
        std::string err;
        if (!SidecarStart(&err)) Log(MML_LOG_ERROR, "host", "sidecar: not started again: %s", err.c_str());
    }
    return 0;
}

} // namespace

// Starts the sidecar (the section made, the rings emptied, the halves that should be loaded sent again).
bool SidecarStart(std::string *err) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_started) return true;
    if (!MakeSection(err)) { g_state = "not started: " + *err; return false; }
    std::string exe = MmlDir() + "sidecar\\mml-sidecar64.exe";
    if (!PathExists(exe)) {
        *err = "no " + exe + " (the 64-bit build: cmake --build --preset x64-release, then deploy)";
        g_state = "not started: " + *err;
        return false;
    }
    {
        std::lock_guard<std::mutex> olk(g_outMx);
        g_out.ring->head = g_out.ring->tail = 0;   // (no sidecar reads or writes now)
        g_in.ring->head = g_in.ring->tail = 0;
    }
    if (!g_job) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    std::wstring cmd = L"\"" + Wide(exe) + L"\" --game " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring dir = Wide(MmlDir() + "sidecar");
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        *err = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        g_state = "not started: " + *err;
        return false;
    }
    if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess))
        HWARN("sidecar: not in a job (%lu): it still ends with the game, by waiting on it", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_pid = pi.dwProcessId;
    g_started = true;
    g_wanted = true;
    g_restarts.push_back(GetTickCount64());
    g_state = "starting";
    CreateThread(nullptr, 0, Watcher, g_proc, 0, nullptr);
    Send(side::kHello, 0, Hello(), 1000);
    for (auto &kv : g_halves) {
        kv.second.state = "loading";
        Send(side::kModLoad, 0, LoadMsg(kv.first, kv.second), 1000);
    }
    HLOG("sidecar: mml-sidecar64 started (pid %lu)", g_pid);
    return true;
}

void SidecarModLoad(const std::string &id, const std::string &dir, const std::string &entry64, const std::string &dataDir, unsigned generation) {
    std::string err;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        HalfState &h = g_halves[id];
        h.dir = dir;
        h.entry = entry64;
        h.dataDir = dataDir;
        h.generation = generation;
        h.state = "loading";
        h.error.clear();
        h.where.clear();
        h.subs.clear();
        UpdateSubs();
        if (g_started) { Send(side::kModLoad, 0, LoadMsg(id, h), 2000); return; }
    }
    if (!SidecarStart(&err)) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_halves[id].state = "not loaded";
        g_halves[id].error = err;
        HWARN("mod %s: its 64-bit half not loaded: %s", id.c_str(), err.c_str());
    }
}

void SidecarModUnload(const std::string &id, const char *why) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_halves.erase(id)) return;
    UpdateSubs();
    for (auto it = g_commands.begin(); it != g_commands.end();) it = it->first.rfind(id + ".", 0) == 0 ? g_commands.erase(it) : std::next(it);
    if (g_started) Send(side::kModUnload, 0, {{"id", id}, {"why", why}}, 2000);
}

// An event for the halves subscribed to its type (never waits: a full ring drops it).
void SidecarEvent(const char *type, const std::string &dataJson) {
    if (!g_base || !SidecarWants(type)) return;
    json d = json::parse(dataJson.empty() ? "{}" : dataJson, nullptr, false);
    if (!d.is_object()) d = json::object();
    d["type"] = type;
    if (Send(side::kEvent, 0, d, 0)) g_sent++;
    else g_dropped++;
}
void SidecarTick(float dt) {
    if (g_tickSub) SidecarEvent("MISSION_TICK", Dump({{"dt", dt}}));
}
void SidecarFrame(float dt) {
    if (g_frameSub) SidecarEvent("FRAME", Dump({{"dt", dt}, {"frame", Frames()}}));
}
void SidecarAdapter(uint32_t low, uint32_t high) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_haveLuid && g_luid[0] == low && g_luid[1] == high) return;
    g_luid[0] = low;
    g_luid[1] = high;
    g_haveLuid = true;
    if (g_started) Send(side::kAdapter, 0, {{"luid", json::array({low, high})}}, 1000);
}

// A bridge call to a 64-bit half's command (from the bridge's thread). false: not one.
bool SidecarCommand(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    json params = json::parse(paramsJson.empty() ? "{}" : paramsJson, nullptr, false);
    if (params.is_discarded()) params = json::object();
    std::string mod, name;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_commands.count(method)) return false;
        size_t dot = method.find('.');
        mod = method.substr(0, dot);
        name = method.substr(dot + 1);
    }
    Wait w;
    uint32_t seq = g_seq++;
    {
        std::lock_guard<std::mutex> lk(g_waitMx);
        g_waits[seq] = &w;
    }
    int timeout = params.is_object() ? params.value("timeout_ms", 5000) : 5000;
    bool sent = Send(side::kCommand, seq, {{"mod", mod}, {"name", name}, {"args", params}}, static_cast<uint32_t>(timeout));
    std::unique_lock<std::mutex> lk(g_waitMx);
    if (sent) g_waitCv.wait_for(lk, std::chrono::milliseconds(timeout), [&] { return w.done; });
    g_waits.erase(seq);
    *ok = w.done && w.ok;
    *reply = w.done ? (w.text.empty() ? "null" : w.text) : sent ? "the sidecar did not answer in " + std::to_string(timeout) + " ms" : "the link to the sidecar is full";
    return true;
}

namespace {
json StateValue() {
    std::lock_guard<std::mutex> lk(g_mx);
    json halves = json::object();
    for (auto &kv : g_halves) {
        json h = {{"state", kv.second.state}, {"entry", kv.second.entry}, {"generation", kv.second.generation},
                  {"subscribed", std::vector<std::string>(kv.second.subs.begin(), kv.second.subs.end())}};
        if (!kv.second.error.empty()) h["error"] = kv.second.error;
        if (!kv.second.where.empty()) h["where"] = kv.second.where;
        auto t = g_textures.find(kv.first);
        if (t != g_textures.end() && !t->second.empty()) {
            json tex = json::object();
            for (auto &x : t->second) tex[x.first] = {{"width", x.second.w}, {"height", x.second.h}, {"format", x.second.format}, {"generation", x.second.generation}};
            h["textures"] = tex;
        }
        halves[kv.first] = h;
    }
    json cmds = json::array();
    for (auto &kv : g_commands) cmds.push_back({{"name", kv.first}, {"help", kv.second}});
    json j = {{"state", g_state}, {"running", g_started}, {"halves", halves}, {"commands", cmds},
              {"events_sent", g_sent}, {"events_dropped", g_dropped}, {"restarts_last_minute", g_restarts.size()}};
    if (g_started) j["pid"] = g_pid;
    if (g_haveLuid) j["adapter_luid"] = json::array({g_luid[0], g_luid[1]});
    return j;
}
} // namespace
std::string SidecarState() { return Dump(StateValue()); }
std::string SidecarHalf(const std::string &id) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_halves.find(id);
    if (it == g_halves.end()) return std::string();
    json h = {{"state", it->second.state}, {"entry", it->second.entry}, {"generation", it->second.generation}};
    if (!it->second.error.empty()) h["error"] = it->second.error;
    if (!it->second.where.empty()) h["where"] = it->second.where;
    return Dump(h);
}
bool SidecarWants(const char *type) {
    if (!g_anySub) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    return g_started && (g_subTypes.count("*") || g_subTypes.count(type));
}
std::vector<std::pair<std::string, std::string>> SidecarCommands() {
    std::lock_guard<std::mutex> lk(g_mx);
    return std::vector<std::pair<std::string, std::string>>(g_commands.begin(), g_commands.end());
}

// A half's shared texture, for its 32-bit half (gfx.cpp opens it on its device): false, none of that name. The handle
// kept here is closed when the half makes the texture anew, so the caller gets a copy of its own.
bool SidecarTexture(const std::string &mod, const std::string &name, SidecarTex *out, bool dup) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto m = g_textures.find(mod);
    if (m == g_textures.end()) return false;
    auto t = m->second.find(name);
    if (t == m->second.end() || !t->second.handle) return false;
    const SharedTex &s = t->second;
    *out = SidecarTex{nullptr, s.w, s.h, s.format, s.generation, s.stamp};
    if (dup) {
        HANDLE mine = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), s.handle, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
        out->handle = mine;
    }
    return true;
}

// ---- the 32-bit halves' channels: the same memory as the 64-bit half's channel_open (sidecar/channel.h), named by
// the game's pid, the mod and the channel; they work whether or not the sidecar runs (either half may open first).
namespace {
std::mutex g_chMx;
std::vector<MmlChannel *> g_channels;
} // namespace

MmlChannel *SidecarChannelOpen(MmlMod *mod, const char *name, uint32_t size, std::string *err) {
    auto *c = new MmlChannel{};
    c->mod = mod;
    if (!side::ChannelOpen(&c->ch, GetCurrentProcessId(), ModIdOf(mod), name, size, err)) {
        delete c;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_chMx);
    g_channels.push_back(c);
    return c;
}
int SidecarChannelWrite(MmlChannel *ch, const void *data, uint32_t bytes) {
    return ch && (data || !bytes) && side::ChannelWrite(&ch->ch, data, bytes) ? 1 : 0;
}
uint32_t SidecarChannelRead(MmlChannel *ch, void *data, uint32_t cap, uint64_t *version) {
    if (!ch || (!data && cap)) {
        if (version) *version = 0;
        return 0;
    }
    return side::ChannelRead(&ch->ch, data, cap, version);
}
uint64_t SidecarChannelWait(MmlChannel *ch, uint64_t after, uint32_t timeoutMs) {
    return ch ? side::ChannelWait(&ch->ch, after, timeoutMs) : 0;
}
void SidecarChannelsForget(MmlMod *mod) {
    std::lock_guard<std::mutex> lk(g_chMx);
    for (MmlChannel *&c : g_channels)
        if (c->mod == mod) {
            side::ChannelClose(&c->ch);
            delete c;
            c = nullptr;
        }
    g_channels.erase(std::remove(g_channels.begin(), g_channels.end(), nullptr), g_channels.end());
}

void SidecarShutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_quitting = true;
        g_wanted = false;
        if (!g_started) return;
    }
    Send(side::kQuit, 0, json(), 200);
}

bool SidecarMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "sidecar.restart") return false;
    *ok = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_restarts.clear();
        if (g_started && g_proc) {
            g_wanted = false;
            TerminateProcess(g_proc, 0xDEAD);
        }
    }
    for (int i = 0; i < 100; i++) {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_started) break;
        }
        Sleep(20);
    }
    std::string err;
    if (!SidecarStart(&err)) { *reply = "sidecar.restart: " + err; return true; }
    *ok = true;
    *reply = SidecarState();
    return true;
}

} // namespace host
