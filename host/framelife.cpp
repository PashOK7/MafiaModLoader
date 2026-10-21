// A frame's release told (mml.h frame_watch; docs/MODDING.md "A frame's release").
//
// LS3D's frames and meshes are counted references (the count at +4 in both builds); every frame's Release is one
// shared function (game_build.h frameRelease, stdcall(this) -> the count), the meshes' another (meshRelease). A mod
// that keeps a frame (a model, its bones: a ragdoll's) asks to be told before it goes: both are hooked once a mod
// watches anything, and a Release that finds the count at 1 (the last reference: its memory goes in it) calls the
// watchers of that frame first, on the thread that releases it (the game's), behind each mod's fault guard; the watch
// ends there. mafia-ragdoll's frame_life.cpp kept the freed ones for a purge at its next tick; told before the free,
// a mod forgets the frame while it is still valid.
//
// Bridge methods (game thread): frame.watch {frame, clear} (the release recorded as the event FRAME_RELEASED),
// frames.watched.
#include "host.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kRefCount = 4;

struct Watch { MmlMod *mod; MmlFrameGoneFn fn; void *user; };   // mod null, fn null: the bridge's (an event)
CRITICAL_SECTION g_lock;
bool g_lockMade;
std::unordered_map<void *, std::vector<Watch>> g_watch;
volatile LONG g_count;      // frames watched (read without the lock: the hooks' fast way out)
bool g_hooked;
std::string g_why = "not needed yet (nothing watched)";
unsigned g_told, g_faults;

using ReleaseFn = unsigned long(__stdcall *)(void *self);
ReleaseFn oFrameRelease, oMeshRelease;

struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

struct GoneCall { const Watch *w; void *frame; };
void CallGone(void *p) {
    auto *c = static_cast<GoneCall *>(p);
    c->w->fn(c->frame, c->w->user);
}

std::string Hex(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

// the last reference going: the frame's watchers told (and forgotten) before LS3D frees it
void Going(void *self) {
    if (!g_count || !self || *reinterpret_cast<volatile LONG *>(static_cast<uint8_t *>(self) + kRefCount) != 1) return;
    std::vector<Watch> ws;
    {
        Lock l;
        auto it = g_watch.find(self);
        if (it == g_watch.end()) return;
        ws = std::move(it->second);
        g_watch.erase(it);
        InterlockedExchange(&g_count, static_cast<LONG>(g_watch.size()));
    }
    for (const Watch &w : ws) {
        g_told++;
        if (!w.fn) {
            EventsRecord("FRAME_RELEASED", json({{"frame", Hex(self)}}).dump(-1, ' ', false, json::error_handler_t::replace));
            continue;
        }
        if (!ModAlive(w.mod)) continue;
        GoneCall c{&w, self};
        if (!ModCall(w.mod, CallGone, &c, "its frame watch")) g_faults++;
    }
}

unsigned long __stdcall hkFrameRelease(void *self) {
    Going(self);
    return oFrameRelease(self);
}
unsigned long __stdcall hkMeshRelease(void *self) {
    Going(self);
    return oMeshRelease(self);
}

bool Ensure(std::string *err) {
    if (g_hooked) return true;
    uintptr_t ls3d = ModuleBase("LS3DF.dll");
    if (!game::g_ls3d || !ls3d || !game::Ls3d().frameRelease || !game::Ls3d().meshRelease) {
        *err = g_why = "the frames' Release is not known for this build";
        return false;
    }
    std::string e;
    bool a = HookAdd(nullptr, ls3d + game::Ls3d().frameRelease, reinterpret_cast<void *>(&hkFrameRelease),
                     reinterpret_cast<void **>(&oFrameRelease), &e);
    bool b = a && HookAdd(nullptr, ls3d + game::Ls3d().meshRelease, reinterpret_cast<void *>(&hkMeshRelease),
                          reinterpret_cast<void **>(&oMeshRelease), &e);
    if (!a || !b) { *err = g_why = "the frames' Release not hooked: " + e; return false; }
    g_hooked = true;
    g_why = "hooked";
    return true;
}

bool Readable(const void *p) {
    MEMORY_BASIC_INFORMATION mi{};
    return p && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

} // namespace

void FrameLifeInit() {
    if (!g_lockMade) { InitializeCriticalSection(&g_lock); g_lockMade = true; }
}

int FrameWatch(MmlMod *mod, void *frame, MmlFrameGoneFn fn, void *user, bool bridge, std::string *err) {
    if (!g_lockMade) FrameLifeInit();
    if (!frame) { *err = "no frame"; return 0; }
    if (!fn && !bridge) {   // the mod's watch on this frame taken away
        Lock l;
        auto it = g_watch.find(frame);
        if (it != g_watch.end()) {
            auto &v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Watch &w) { return w.mod == mod && w.fn; }), v.end());
            if (v.empty()) g_watch.erase(it);
        }
        InterlockedExchange(&g_count, static_cast<LONG>(g_watch.size()));
        return 1;
    }
    if (!Readable(frame) || !Readable(static_cast<uint8_t *>(frame) + kRefCount)) { *err = "not a readable object"; return 0; }
    if (!Ensure(err)) return 0;
    Lock l;
    auto &v = g_watch[frame];
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Watch &w) { return bridge ? !w.fn : (w.mod == mod && w.fn); }), v.end());
    if (bridge) v.push_back({nullptr, nullptr, nullptr});
    else v.push_back({mod, fn, user});
    InterlockedExchange(&g_count, static_cast<LONG>(g_watch.size()));
    return 1;
}

void FrameLifeModGone(MmlMod *mod) {
    if (!g_lockMade) return;
    Lock l;
    for (auto it = g_watch.begin(); it != g_watch.end();) {
        auto &v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Watch &w) { return w.mod == mod && w.fn; }), v.end());
        it = v.empty() ? g_watch.erase(it) : std::next(it);
    }
    InterlockedExchange(&g_count, static_cast<LONG>(g_watch.size()));
}

bool FrameLifeMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "frame.watch" && method != "frames.watched") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "frames.watched") {
            json list = json::array();
            {
                Lock l;
                for (const auto &e : g_watch) {
                    json who = json::array();
                    for (const Watch &w : e.second) who.push_back(w.fn ? ModIdOf(w.mod) : "bridge");
                    list.push_back({{"frame", Hex(e.first)}, {"by", who}});
                }
            }
            *reply = dump({{"frames", list}, {"release", g_why}, {"told", g_told}, {"faults", g_faults}});
            *ok = true;
            return true;
        }
        std::string f = p.value("frame", "");
        char *end = nullptr;
        void *frame = reinterpret_cast<void *>(static_cast<uintptr_t>(strtoul(f.c_str(), &end, 16)));
        if (f.empty() || (end && *end) || !frame) { *reply = "frame.watch: frame: an address (\"0x...\")"; return true; }
        if (p.value("clear", false)) {
            Lock l;
            auto it = g_watch.find(frame);
            if (it != g_watch.end()) {
                auto &v = it->second;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const Watch &w) { return !w.fn; }), v.end());
                if (v.empty()) g_watch.erase(it);
            }
            InterlockedExchange(&g_count, static_cast<LONG>(g_watch.size()));
            *reply = dump({{"frame", Hex(frame)}, {"watched", false}});
            *ok = true;
            return true;
        }
        std::string err;
        if (!FrameWatch(nullptr, frame, nullptr, nullptr, true, &err)) { *reply = "frame.watch: " + err; return true; }
        *reply = dump({{"frame", Hex(frame)}, {"watched", true}, {"refs", *reinterpret_cast<volatile LONG *>(static_cast<uint8_t *>(frame) + kRefCount)}});
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
