// The core mod: the game's live state for agents and tools (commands without a prefix: `player`, `humans`...).
//
// It is a mod like any other (the same C ABI, hot-reloadable with `mod.reload core`), so the game knowledge it
// serves grows without restarting the game. Every read of the game's memory goes through the host's mem_read (never
// faults): a stale pointer in a query is an error in the reply, not a crash.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>
#include "json.hpp"
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace {
const MmlHost *H;
MmlMod *M;
std::unordered_set<uintptr_t> g_humans;   // every C_human constructed (kept across reloads in the mod's state)

template <class T> bool Rd(uintptr_t a, T *v) { return a && H->mem_read(a, v, sizeof(T)) != 0; }
template <class T> T R(uintptr_t a, T def = T()) { T v; return Rd(a, &v) ? v : def; }

std::string Hex(uintptr_t v) { char b[16]; snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(v)); return b; }
json Vec(const float *v) { return json::array({v[0], v[1], v[2]}); }

std::string Str(uintptr_t p, size_t max = 128) {
    if (!p) return "";
    char buf[256] = {};
    if (!H->mem_read(p, buf, max < sizeof buf - 1 ? max : sizeof buf - 1)) return "";
    return buf;
}

json Frame(uintptr_t f) {
    if (!f) return nullptr;
    float w[16];
    if (!H->mem_read(f + game::kFrameWorld, w, sizeof w)) return {{"addr", Hex(f)}, {"error", "unreadable"}};
    uintptr_t parent = R<uintptr_t>(f + game::kFrameParent);
    return {{"addr", Hex(f)}, {"name", Str(R<uintptr_t>(f + game::kFrameName))}, {"type", R<uint32_t>(f + game::kFrameType)},
            {"flags", Hex(R<uint32_t>(f + game::kFrameFlags))}, {"pos", Vec(w + 12)},
            {"axes", {Vec(w), Vec(w + 4), Vec(w + 8)}}, {"parent", Hex(parent)},
            {"parent_name", Str(R<uintptr_t>(parent + game::kFrameName))}};
}

json Actor(uintptr_t a) {
    uintptr_t frame = R<uintptr_t>(a + game::kActorFrame);
    float pos[3] = {}, facing[3] = {};
    H->mem_read(a + game::kActorPos, pos, sizeof pos);
    H->mem_read(a + game::kActorFacing, facing, sizeof facing);
    json j = {{"actor", Hex(a)}, {"vtable", Hex(R<uintptr_t>(a))}, {"type", R<uint32_t>(a + game::kActorType)},
              {"alive", R<uint8_t>(a + game::kActorAlive) != 0}, {"pos_local", Vec(pos)}, {"facing", Vec(facing)}};
    if (frame) {
        float w[16];
        if (H->mem_read(frame + game::kFrameWorld, w, sizeof w)) j["pos"] = Vec(w + 12);
        j["model"] = Str(R<uintptr_t>(frame + game::kFrameName));
        j["frame"] = Hex(frame);
    }
    return j;
}

uintptr_t Player() {
    uintptr_t mission = R<uintptr_t>(game::Exe().missionPtr);
    uintptr_t g = R<uintptr_t>(mission + game::kMissionGame);
    return R<uintptr_t>(g + game::kGamePlayer);
}

json Params(const char *args) {
    try { json p = json::parse(args ? args : "null"); return p.is_object() ? p : json::object(); }
    catch (...) { return json::object(); }
}
int Reply(MmlReply *r, const json &j) {
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    H->reply_append(r, s.c_str(), s.size());
    return 0;
}
int Fail(MmlReply *r, const char *msg) { H->reply_error(r, msg); return 1; }
bool Known() { return game::g_exe != nullptr; }

// ---- commands
int CmdGame(const char *, MmlReply *r, void *) {
    json j = {{"build", H->game_build()}, {"game_dir", H->game_dir()}, {"exe_base", Hex(H->module_base(nullptr))},
              {"ls3d_base", Hex(H->module_base("LS3DF.dll"))}};
    if (Known()) {
        uintptr_t mission = R<uintptr_t>(game::Exe().missionPtr);
        j["mission"] = Hex(mission);
        j["scene"] = Hex(R<uintptr_t>(mission + game::kMissionScene));
        j["player"] = Hex(Player());
    }
    return Reply(r, j);
}

int CmdPlayer(const char *, MmlReply *r, void *) {
    if (!Known()) return Fail(r, "not a known Mafia build");
    uintptr_t p = Player();
    if (!p) return Fail(r, "no player (no mission loaded)");
    json j = Actor(p);
    j["health"] = R<float>(p + game::kHumanHealth);
    uintptr_t veh = R<uintptr_t>(p + 0x98);   // the vehicle he uses (a car, a tram)
    j["vehicle"] = veh ? json{{"actor", Hex(veh)}, {"type", R<uint32_t>(veh + game::kActorType)}} : json(nullptr);
    return Reply(r, j);
}

int CmdHumans(const char *args, MmlReply *r, void *) {
    json p = Params(args);
    float radius = p.value("radius", 0.0f);
    float pp[3] = {};
    uintptr_t player = Known() ? Player() : 0;
    if (radius > 0 && player) {
        uintptr_t f = R<uintptr_t>(player + game::kActorFrame);
        float w[16];
        if (f && H->mem_read(f + game::kFrameWorld, w, sizeof w)) { pp[0] = w[12]; pp[1] = w[13]; pp[2] = w[14]; }
    }
    json out = json::array();
    for (uintptr_t h : g_humans) {
        json a = Actor(h);
        if (radius > 0 && a.contains("pos")) {
            float dx = a["pos"][0].get<float>() - pp[0], dy = a["pos"][1].get<float>() - pp[1], dz = a["pos"][2].get<float>() - pp[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > radius) continue;
            a["distance"] = d;
        }
        a["health"] = R<float>(h + game::kHumanHealth);
        a["player"] = h == player;
        out.push_back(a);
    }
    return Reply(r, out);
}

bool SafeListActors(std::vector<void *> *out) {
    __try { game::ListActors(out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int CmdActors(const char *, MmlReply *r, void *) {
    if (!Known()) return Fail(r, "not a known Mafia build");
    std::vector<void *> all;
    if (!SafeListActors(&all)) return Fail(r, "the actor lists could not be read");
    json out = json::array();
    for (void *a : all) out.push_back(Actor(reinterpret_cast<uintptr_t>(a)));
    return Reply(r, out);
}

int CmdFrame(const char *args, MmlReply *r, void *) {
    json p = Params(args);
    std::string a = p.value("addr", "");
    uintptr_t f = strtoul(a.c_str(), nullptr, 16);
    if (!f) return Fail(r, "addr: the frame's address (hex)");
    json j = Frame(f);
    return Reply(r, j);
}

bool SafeEnumDeep(void *root, std::vector<void *> *out) {
    __try { game::EnumFramesDeep(root, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int CmdFramesFind(const char *args, MmlReply *r, void *) {
    if (!Known()) return Fail(r, "not a known Mafia build");
    json p = Params(args);
    std::string needle = p.value("name", "");
    size_t limit = p.value("limit", 50u);
    uintptr_t mission = R<uintptr_t>(game::Exe().missionPtr);
    uintptr_t scene = R<uintptr_t>(mission + game::kMissionScene);
    uintptr_t player = Player();
    // the root above the player's model (the scene's primary sector and everything under it)
    uintptr_t root = player ? R<uintptr_t>(player + game::kActorFrame) : 0;
    for (int i = 0; i < 32 && root && R<uintptr_t>(root + game::kFrameParent); i++) root = R<uintptr_t>(root + game::kFrameParent);
    if (!root) return Fail(r, "no scene root (no mission?)");
    std::vector<void *> frames;
    if (!SafeEnumDeep(reinterpret_cast<void *>(root), &frames)) return Fail(r, "the scene could not be walked");
    json out = json::array();
    for (void *f : frames) {
        std::string n = Str(R<uintptr_t>(reinterpret_cast<uintptr_t>(f) + game::kFrameName));
        if (!needle.empty() && n.find(needle) == std::string::npos) continue;
        out.push_back(Frame(reinterpret_cast<uintptr_t>(f)));
        if (out.size() >= limit) break;
    }
    return Reply(r, json{{"root", Hex(root)}, {"scene", Hex(scene)}, {"frames_walked", frames.size()}, {"found", out}});
}

// ---- events: the human registry
void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_HUMAN_CREATED) g_humans.insert(reinterpret_cast<uintptr_t>(ev->actor));
    else if (ev->type == MML_EVENT_ACTOR_DESTROYED) g_humans.erase(reinterpret_cast<uintptr_t>(ev->actor));
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (uintptr_t ls3d = host->module_base("LS3DF.dll")) game::Detect(ls3d);
    // the registry from the previous load of this mod (a reload keeps it)
    size_t n = host->state_get(mod, "humans", nullptr, 0);
    if (n) {
        std::vector<uintptr_t> v(n / sizeof(uintptr_t));
        host->state_get(mod, "humans", v.data(), n);
        g_humans.insert(v.begin(), v.end());
    }
    host->subscribe(mod, MML_EVENT_HUMAN_CREATED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_ACTOR_DESTROYED, OnEvent, nullptr);
    host->command(mod, "game", "the build, module bases, the mission, the scene and the player", CmdGame, nullptr);
    host->command(mod, "player", "the player: position, facing, health, alive, the vehicle he is in", CmdPlayer, nullptr);
    host->command(mod, "humans", "every C_human alive in memory (params: radius m round the player)", CmdHumans, nullptr);
    host->command(mod, "actors", "the mission's actor lists: type, model, position", CmdActors, nullptr);
    host->command(mod, "frame", "an LS3D frame: name, type, world position and axes, parent (params: addr)", CmdFrame, nullptr);
    host->command(mod, "frames.find", "frames of the scene whose name contains `name` (params: name, limit)", CmdFramesFind, nullptr);
    host->log(mod, MML_LOG_INFO, "core: %zu humans known, game %s", g_humans.size(), host->game_build());
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *mod) {
    std::vector<uintptr_t> v(g_humans.begin(), g_humans.end());
    H->state_put(mod, "humans", v.data(), v.size() * sizeof(uintptr_t));
    g_humans.clear();
}
