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
        j["mission_name"] = Str(R<uintptr_t>(mission + game::Exe().missionName));
    }
    if (MML_HAS(H, mission_state)) {
        static const char *kState[] = {"none", "loading", "running", "paused"};
        int st = H->mission_state();
        j["mission_state"] = st >= 0 && st < 4 ? kState[st] : "?";
    }
    return Reply(r, j);
}

// ---- the car (game.h `Car`: the same fields, read here through mem_read so a stale pointer is an error, not a fault)
std::vector<uint8_t> g_carState;   // `car.save`'s
uintptr_t g_carStateOf;
std::string g_carStateModel;       // its car's frame's name (told in the replies)

// The car asked for: {"car": "0x..."} or the one the player drives now, else the one he drove last (the HUD's).
uintptr_t CarOf(const json &p, const char **which) {
    std::string a = p.value("car", "");
    if (!a.empty()) { *which = "given"; return strtoul(a.c_str(), nullptr, 16); }
    uintptr_t player = Player(), v = R<uintptr_t>(player + game::kHumanVehicle);
    if (v && R<int>(v + game::kActorType) == game::kActorTypeCar) { *which = "driven"; return v; }
    *which = "last";
    return game::Exe().hudCar ? R<uintptr_t>(game::Exe().hud + game::Exe().hudCar) : 0;
}

int CmdCar(const char *args, MmlReply *r, void *) {
    if (!Known() || !game::Exe().vehPos) return Fail(r, "the car is not known for this build");
    const game::ExeBuild &e = game::Exe();
    const char *which = "";
    uintptr_t car = CarOf(Params(args), &which);
    if (!car) return Fail(r, "no car (the player has not driven one in this mission)");
    uintptr_t v = car + e.vehicle;
    float pos[3], vel[3];
    if (R<int>(car + game::kActorType, -1) != game::kActorTypeCar || !H->mem_read(v + e.vehPos, pos, sizeof pos) ||
        !H->mem_read(v + e.vehVelocity, vel, sizeof vel))
        return Fail(r, "not a car (the address is not an actor of type 4, or cannot be read)");
    float speed = R<float>(v + e.vehSpeed), steerMax = R<float>(v + e.vehSteerMax), steer = R<float>(v + e.vehSteer);
    float health = R<float>(v + e.vehEngineHealth), healthMax = R<float>(v + e.vehEngineHealthMax);
    bool engine = R<uint8_t>(v + e.vehEngineOn) != 0;
    uintptr_t frame = R<uintptr_t>(car + game::kActorFrame);
    json j = {{"car", Hex(car)}, {"which", which}, {"model", Str(R<uintptr_t>(frame + game::kFrameName))}, {"pos", Vec(pos)},
              {"velocity", Vec(vel)}, {"speed", speed}, {"speed_kmh", speed * 3.6f}, {"speed_mph", speed * 2.2369363f},
              {"steering", steerMax > 1e-4f ? steer / steerMax : 0.0f}, {"steer_angle", steer},
              {"steer_wanted", R<float>(v + e.vehSteerWanted)}, {"steer_max", steerMax},
              {"gear", R<int>(v + e.vehGear)}, {"gear_max", R<int>(v + e.vehGearMax)},
              {"rpm", engine ? R<float>(v + e.vehRps) * 60.0f : 0.0f}, {"engine_on", engine},
              {"fuel", R<float>(v + e.vehFuel)}, {"fuel_tank", R<float>(v + e.vehFuelTank)},
              {"engine_health", healthMax > 1e-4f ? health / healthMax : 0.0f},
              {"damage", healthMax > 1e-4f ? 1.0f - health / healthMax : 1.0f}};
    if (e.vehStateSize) j["state_bytes"] = game::CarStateSize(reinterpret_cast<void *>(car));   // (grows with the body's dents)
    float w[16];
    if (frame && H->mem_read(frame + game::kFrameWorld, w, sizeof w)) j["axes"] = {Vec(w), Vec(w + 4), Vec(w + 8)};
    if (e.vehAxes) {   // the rigid body as the physics keeps it (game.h Car; knowledge/notes/vehicle_physics.md)
        float ax[9], turn[3];
        uint32_t flags = R<uint32_t>(v + e.vehicleFlags);
        json body = {{"awake", (flags & game::kVehAwake) != 0}, {"calm", (flags & game::kVehCalm) != 0},
                     {"flags", Hex(flags)}, {"step_s", R<float>(v + e.vehicleStepTime)}, {"substep_max_s", R<float>(v + e.vehSubstepMax)}};
        if (H->mem_read(v + e.vehTurn, turn, sizeof turn)) body["angular_velocity"] = Vec(turn);
        if (H->mem_read(v + e.vehAxes, ax, sizeof ax)) body["forward"] = Vec(ax), body["right"] = Vec(ax + 3), body["up"] = Vec(ax + 6);
        j["body"] = body;
        json wheels = json::array();
        uintptr_t list = R<uintptr_t>(v + e.vehWheels);
        for (int i = 0, n = R<int>(v + e.vehWheelCount); list && i < n && i < 16; i++) {
            uintptr_t wh = R<uintptr_t>(list + 4 * i);
            if (!wh) continue;
            uint32_t wf = R<uint32_t>(wh + game::kWheelFlags);
            wheels.push_back({{"wheel", Hex(wh)}, {"frame", Str(R<uintptr_t>(R<uintptr_t>(wh + game::kWheelFrame) + game::kFrameName))},
                              {"on_ground", (wf & game::kWheelOnGround) != 0}, {"load_n", R<float>(wh + game::kWheelLoad)},
                              {"force_n", R<float>(wh + game::kWheelForce)}, {"compression_m", R<float>(wh + game::kWheelCompression)},
                              {"free_length_m", R<float>(wh + game::kWheelFreeLength)}, {"stiffness", R<float>(wh + game::kWheelStiffness)},
                              {"flags", Hex(wf)}});
        }
        j["wheels"] = wheels;
    }
    return Reply(r, j);
}

bool SafeCarSave(void *car, std::vector<uint8_t> *out) {
    __try { return game::CarStateSave(car, out); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeCarLoad(void *car, const std::vector<uint8_t> *state) {
    __try { return game::CarStateLoad(car, *state); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string ModelOf(uintptr_t car) { return Str(R<uintptr_t>(R<uintptr_t>(car + game::kActorFrame) + game::kFrameName)); }

int CmdCarSave(const char *args, MmlReply *r, void *) {
    if (!Known() || !game::Exe().vehStateSave) return Fail(r, "a car's state is not known for this build");
    const char *which = "";
    uintptr_t car = CarOf(Params(args), &which);
    if (!car || R<int>(car + game::kActorType, -1) != game::kActorTypeCar) return Fail(r, "no car");
    if (!SafeCarSave(reinterpret_cast<void *>(car), &g_carState)) { g_carState.clear(); return Fail(r, "the game's SaveVehicleState failed"); }
    g_carStateOf = car;
    g_carStateModel = ModelOf(car);
    return Reply(r, json{{"car", Hex(car)}, {"which", which}, {"model", g_carStateModel}, {"bytes", g_carState.size()}});
}
int CmdCarRestore(const char *args, MmlReply *r, void *) {
    if (g_carState.empty()) return Fail(r, "nothing saved (car.save first)");
    const char *which = "";
    uintptr_t car = CarOf(Params(args), &which);
    if (!car || R<int>(car + game::kActorType, -1) != game::kActorTypeCar) return Fail(r, "no car");
    std::string model = ModelOf(car);   // (each car's frame has a name of its own: told, not compared)
    if (!game::CarStateFits(reinterpret_cast<void *>(car), g_carState))
        return Fail(r, ("the state was saved from " + g_carStateModel + ", built otherwise than " + model +
                        " (its lights, body parts, seats or wheels differ): a state goes back into its model only").c_str());
    if (!SafeCarLoad(reinterpret_cast<void *>(car), &g_carState)) return Fail(r, "not put back: the game's LoadVehicleState failed");
    return Reply(r, json{{"car", Hex(car)}, {"which", which}, {"model", model}, {"bytes", g_carState.size()},
                         {"saved_from", Hex(g_carStateOf)}});
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
    else if (ev->type == MML_EVENT_MISSION_CLOSE) { g_carState.clear(); g_carStateOf = 0; }   // its car is going
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (uintptr_t ls3d = host->module_base("LS3DF.dll")) game::Detect(ls3d);
    if (size_t k = host->state_get(mod, "car.state", nullptr, 0)) {   // `car.save`'s, from the previous load
        g_carState.resize(k);
        host->state_get(mod, "car.state", g_carState.data(), k);
        host->state_get(mod, "car.of", &g_carStateOf, sizeof g_carStateOf);
        g_carStateModel.resize(host->state_get(mod, "car.model", nullptr, 0));
        host->state_get(mod, "car.model", g_carStateModel.data(), g_carStateModel.size());
    }
    // the registry from the previous load of this mod (a reload keeps it)
    size_t n = host->state_get(mod, "humans", nullptr, 0);
    if (n) {
        std::vector<uintptr_t> v(n / sizeof(uintptr_t));
        host->state_get(mod, "humans", v.data(), n);
        g_humans.insert(v.begin(), v.end());
    }
    host->subscribe(mod, MML_EVENT_HUMAN_CREATED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_ACTOR_DESTROYED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->command(mod, "game", "the build, module bases, the mission (its name and state), the scene and the player", CmdGame, nullptr);
    host->command(mod, "player", "the player: position, facing, health, alive, the vehicle he is in", CmdPlayer, nullptr);
    host->command(mod, "car", "the player's car (driven now, else driven last; params: car, an address): position, speed, steering, gear, rpm, fuel, damage; the rigid body (angular velocity, axes, calm, asleep) and each wheel", CmdCar, nullptr);
    host->command(mod, "car.save", "the car's whole state kept in memory, by the game's own SaveVehicleState (params: car)", CmdCarSave, nullptr);
    host->command(mod, "car.restore", "the kept state put back into the car (the same car or the same model): repaired, then LoadVehicleState", CmdCarRestore, nullptr);
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
    // `car.save`'s state, its car and model: kept across this mod's reloads
    H->state_put(mod, "car.state", g_carState.data(), g_carState.size());
    H->state_put(mod, "car.of", &g_carStateOf, sizeof g_carStateOf);
    H->state_put(mod, "car.model", g_carStateModel.data(), g_carStateModel.size());
}
