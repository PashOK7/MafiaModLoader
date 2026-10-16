// The world for agents and mods: actors made new by the game's own functions (a car or a person of any model), moved
// and turned, armed; the game's car list and items found by name (mml.h actor_spawn, actor_remove, actor_teleport,
// item_id, give_item; docs/SCRIPTS.md "The world without a program").
//
// A new actor is made the way the scripts' ACTOR_DUPLICATE makes its copy and DAN_INTERNAL_1 its car (both read in
// both builds' code; game_build.h "Actors made new"): a model frame from the I3D driver, the model file opened into
// it by the game's model cache, the frame named, placed, turned and linked into the sector at its place; the actor
// made by C_mission::CreateActor, initialised on the frame (it then owns the frame: +0x6C, as the duplicate sets
// it), kept out of saved games (+0x65, as the cheat car sets it) and added to the game's temporary actors. A car's
// particles are switched on and the car settled on its wheels (CalmVehicle 100), as the cheat car's. A person is the
// game's NPC kind (27, C_entity) with no script: he stands until a program tells him what to do.
//
// Moving and arming are the scripts' own commands, run at once by the game's interpreter (ScriptExec): actor_setpos
// (with its tails: a car stopped, the lights' sector worked out again), car_repair, human_addweapon. Turning is what
// ACTOR_SETDIR does with the direction it works out: the actor's facing (+0x30), the frame's SetDir and Update.
//
// Bridge methods (game thread): world.spawn, world.remove {actor}, world.teleport, world.give, world.cars {query},
// world.items {query}, world.spawned (docs/PROTOCOL.md).
#include "host.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "json.hpp"
#include "mml/game/game.h"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

enum { kItemSize = 188, kActorOwnsFrame = 0x6C, kActorNotSaved = 0x65, kFrameModel = 9 };
enum { kVtSetDir = 0x0C, kVtUpdate = 0x18, kVtSetName = 0x28, kVtLinkTo = 0x2C, kVtActorRelease = 0x44, kVtActorInit = 0x48 };

using CreateFrameFn = void *(__stdcall *)(void *driver, int type);
using OpenFn = int(__fastcall *)(uintptr_t cache, void *edx, void *frame, const char *file, int, int, int, int);
using ReleaseFn = int(__stdcall *)(void *frame);
using SetNameFn = int(__stdcall *)(void *frame, const char *name);
using SetDirFn = int(__stdcall *)(void *frame, const V3 *dir, float roll);
using UpdateFn = int(__stdcall *)(void *frame);
using LinkToFn = int(__stdcall *)(void *frame, void *parent, uint32_t flags);
using SetPosFn = void(__fastcall *)(void *frame, void *edx, const V3 *pos);
using SceneMovedFn = int(__fastcall *)(void *scene, void *edx, void *frame);
using CreateActorFn = void *(__fastcall *)(void *mission, void *edx, int type);
using InitFn = char(__fastcall *)(void *actor, void *edx, void *frame);
using ActorFn = void(__fastcall *)(void *self, void *edx, void *actor);
using ActorReleaseFn = int(__fastcall *)(void *actor, void *edx);
using CalmFn = void(__fastcall *)(void *vehicle, void *edx, int steps);

const game::ExeBuild &E() { return game::Exe(); }
bool SpawnKnown() { return game::g_exe && E().modelCache && E().missionCreateActor && E().gameAddTemp && E().i3dDriver; }

uint32_t R32(uintptr_t a) { uint32_t v = 0; MemRead(a, &v, 4); return v; }
std::string Hex(uintptr_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(v));
    return b;
}
void *Vt(void *obj, int off) { return reinterpret_cast<void *>(R32(R32(reinterpret_cast<uintptr_t>(obj)) + off)); }

uintptr_t g_faultAt;
int Note(EXCEPTION_POINTERS *e) {
    g_faultAt = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (Note(GetExceptionInformation())) { return GetExceptionCode(); }
}
std::string Faulted(const char *what, DWORD ex) {
    return std::string(what) + " faulted (" + Hex(ex) + " at " + DescribeAddress(g_faultAt) + ")";
}

// The actors made here (any mod's, the bridge's): the ones actor_remove may take out.
struct Spawned { void *actor; MmlMod *mod; int kind; std::string model; };
std::vector<Spawned> g_spawned;
std::vector<void *> g_removing;   // taken out by actor_remove, destroyed at the game's next tick
int g_spawnCount;

// A name of the game's (a fixed-size field) as UTF-8: the game's strings are in its language's code page.
std::string Str(uintptr_t at, size_t n) {
    std::string s(n, '\0');
    if (!MemRead(at, &s[0], n)) return "";
    s.resize(strnlen(s.c_str(), n));
    wchar_t w[128];
    char out[384];
    if (s.empty() || MultiByteToWideChar(GameCodePage(), 0, s.c_str(), -1, w, 128) <= 0) return s;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof out, nullptr, nullptr) > 0 ? std::string(out) : s;
}
std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- the game's tables
struct CarRec { int id; std::string name, prefix; };
std::vector<CarRec> Cars() {
    std::vector<CarRec> out;
    if (!game::g_exe || !E().carDb) return out;
    uint32_t n = R32(E().carDb), recs = R32(E().carDb + 4);
    if (!recs || n > 1000) return out;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t r = recs + i * static_cast<uint32_t>(E().carDbStride);
        std::string prefix = Str(r, 32), name = Str(r + 0x60, 32);
        if (prefix.empty() || prefix == "-") continue;   // (record 0 is a placeholder)
        out.push_back({static_cast<int>(i), name, prefix});
    }
    return out;
}
struct ItemRec { int id; std::string name, model; uint32_t flags; int magazine, reserve; };
std::vector<ItemRec> Items() {
    std::vector<ItemRec> out;
    if (!game::g_exe || !E().items) return out;
    uint32_t recs = R32(E().items), n = R32(E().items + 4);
    if (!recs || n > 1000) return out;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t r = recs + i * kItemSize;
        out.push_back({static_cast<int>(i), Str(r, 32), Str(r + 0x24, 32), R32(r + 0x20), static_cast<int>(R32(r + 0x60)),
                       static_cast<int>(R32(r + 0x64))});
    }
    return out;
}
const char *ItemKind(uint32_t f) { return f & 0x80 ? "throwable" : f & 0x20 ? "firearm" : f & 0x04 ? "melee" : "item"; }

// One of a list by its number, its exact name (any case), or a part of its name found in it alone.
template <class T, class Names> int Pick(const std::vector<T> &list, const std::string &want, const char *what, Names names, std::string *err) {
    if (want.empty()) { *err = std::string(what) + ": a name or a number"; return -1; }
    if (std::all_of(want.begin(), want.end(), [](char c) { return isdigit(static_cast<unsigned char>(c)) != 0; })) {
        int id = std::atoi(want.c_str());
        for (const T &t : list) if (t.id == id) return id;
        *err = std::string("no ") + what + " number " + want;
        return -1;
    }
    std::string w = Lower(want);
    std::vector<const T *> part;
    for (const T &t : list)
        for (const std::string &n : names(t)) {
            std::string l = Lower(n);
            if (l == w) return t.id;
            if (l.find(w) != std::string::npos) { part.push_back(&t); break; }
        }
    if (part.size() == 1) return part[0]->id;
    if (part.empty()) { *err = std::string("no ") + what + " named \"" + want + "\""; return -1; }
    *err = std::string("\"") + want + "\" names several " + what + "s:";
    for (size_t i = 0; i < part.size() && i < 12; i++) *err += (i ? ", \"" : " \"") + names(*part[i])[0] + "\"";
    return -1;
}

// The frames C_ShotSkeleton::TestCollision reads without checking (a person's bones; MafiaOnline's list, each name
// seen in the game's own human models): a model without one of them would fault the first shot at it.
const char *const kHumanFrames[] = {"r_hand", "r_elbow", "r_arm", "r_foot", "r_shin", "r_thigh", "l_hand", "l_elbow", "l_arm",
                                    "l_foot", "l_shin", "l_thigh", "base", "back1", "back2", "back3", "neck", "notify"};

V3 Flat(V3 d) {
    d.y = 0;
    float l = std::sqrt(d.x * d.x + d.z * d.z);
    return l > 1e-5f ? V3{d.x / l, 0, d.z / l} : V3{0, 0, 1};
}

// An actor's place and its forward (horizontal, its model's z axis): the car for a player who sits in one.
void *Reference(void *actor) {
    if (actor && actor == game::Player())
        if (void *car = game::PlayerCar()) return car;
    return actor;
}
V3 Forward(void *actor) {
    void *f = game::at<void *>(actor, game::kActorFrame);
    const float *w = f ? game::WorldMatrix(f) : nullptr;
    return Flat(w ? V3{w[8], w[9], w[10]} : game::at<V3>(actor, game::kActorFacing));
}

} // namespace

void *WorldSpawn(MmlMod *mod, int kind, const char *model, const MmlVec3 *pos, const MmlVec3 *dir, std::string *err) {
    if (!SpawnKnown()) { *err = "making actors is not known for this build"; return nullptr; }
    int state = MissionState();
    void *mission = game::Mission();
    if ((state != MML_MISSION_RUNNING && state != MML_MISSION_PAUSED) || !mission || !game::Game()) { *err = "no mission loaded"; return nullptr; }
    if (kind != MML_ACTOR_CAR && kind != MML_ACTOR_HUMAN) { *err = "kind: MML_ACTOR_CAR (4) or MML_ACTOR_HUMAN (27)"; return nullptr; }
    if (!model || !*model || !pos) { *err = "model, pos: needed"; return nullptr; }
    std::string file = model;
    if (kind == MML_ACTOR_CAR && file.find('.') == std::string::npos) {   // a name of the car list
        std::vector<CarRec> cars = Cars();
        int id = Pick(cars, file, "car", [](const CarRec &c) { return std::vector<std::string>{c.name, c.prefix}; }, err);
        if (id < 0) return nullptr;
        for (const CarRec &c : cars) if (c.id == id) file = c.prefix + "00.i3d";
    }
    if (file.find('.') == std::string::npos) file += ".i3d";
    V3 p{pos->x, pos->y, pos->z};
    V3 d = dir ? V3{dir->x, dir->y, dir->z} : V3{0, 0, 1};
    if (kind == MML_ACTOR_HUMAN || !dir) d = Flat(d);
    else {
        float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        d = l > 1e-5f ? V3{d.x / l, d.y / l, d.z / l} : V3{0, 0, 1};
    }
    const game::ExeBuild &x = E();
    void *driver = reinterpret_cast<void *>(R32(x.i3dDriver));
    if (!driver) { *err = "no I3D driver"; return nullptr; }

    // The model frame, from the driver, the file opened into it by the model cache.
    void *frame = nullptr;
    int opened = -1;
    auto open = [&] {
        frame = reinterpret_cast<CreateFrameFn>(Vt(driver, x.drvCreateFrame))(driver, kFrameModel);
        if (frame) opened = reinterpret_cast<OpenFn>(x.modelOpen)(x.modelCache, nullptr, frame, file.c_str(), 0, 0, 0, 0);
    };
    if (DWORD ex = Guarded(open)) { *err = Faulted("opening the model", ex); return nullptr; }
    if (!frame) { *err = "the driver made no frame"; return nullptr; }
    auto release = [&] { reinterpret_cast<ReleaseFn>(Vt(frame, 0))(frame); };
    if (opened != 0) {
        Guarded(release);
        *err = "no model \"" + file + "\" (the game's models\\ folder and archives: a car's file is <its list prefix><colour>.i3d, `world.cars`)";
        return nullptr;
    }
    if (kind == MML_ACTOR_HUMAN) {
        const char *missing = nullptr;
        auto check = [&] { for (const char *n : kHumanFrames) if (!game::FindChild(frame, n)) { missing = n; break; } };
        DWORD ex = Guarded(check);
        if (ex || missing) {
            Guarded(release);
            *err = ex ? Faulted("looking into the model", ex) : "\"" + file + "\" is not a person's model (it has no frame \"" + missing + "\")";
            return nullptr;
        }
    }
    // Named, placed, turned, linked into the sector at its place; then the actor made on it.
    std::string base = file.substr(0, file.rfind('.'));
    size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::string name = base + "_mml" + std::to_string(++g_spawnCount);
    void *actor = nullptr;
    bool inited = false;
    auto make = [&] {
        reinterpret_cast<SetNameFn>(Vt(frame, kVtSetName))(frame, name.c_str());
        reinterpret_cast<SetPosFn>(x.frameSetPos)(frame, nullptr, &p);
        reinterpret_cast<SetDirFn>(Vt(frame, kVtSetDir))(frame, &d, 0.0f);
        reinterpret_cast<UpdateFn>(Vt(frame, kVtUpdate))(frame);
        reinterpret_cast<SceneMovedFn>(x.sceneMoved)(game::at<void *>(mission, game::kMissionScene), nullptr, frame);
        actor = reinterpret_cast<CreateActorFn>(x.missionCreateActor)(mission, nullptr, kind);
        if (!actor) return;
        inited = reinterpret_cast<InitFn>(Vt(actor, kVtActorInit))(actor, nullptr, frame) != 0;
        if (!inited) { reinterpret_cast<ActorReleaseFn>(Vt(actor, kVtActorRelease))(actor, nullptr); return; }
        game::at<uint8_t>(actor, kActorOwnsFrame) = 1;   // the actor's destructor releases the frame (ACTOR_DUPLICATE)
        game::at<uint8_t>(actor, kActorNotSaved) = 1;    // (DAN_INTERNAL_1)
        if (kind == MML_ACTOR_CAR) game::at<uint8_t>(actor, x.carParticles) = 1;
        reinterpret_cast<ActorFn>(x.gameAddTemp)(game::Game(), nullptr, actor);
        if (kind == MML_ACTOR_CAR) reinterpret_cast<CalmFn>(x.vehicleCalm)(&game::at<uint8_t>(actor, x.vehicle), nullptr, 100);
    };
    if (DWORD ex = Guarded(make)) { *err = Faulted("making the actor", ex); return nullptr; }   // (left as it is)
    if (!actor) { Guarded(release); *err = "the mission made no actor of type " + std::to_string(kind); return nullptr; }
    if (!inited) { *err = "the actor's Init refused \"" + file + "\""; return nullptr; }   // (as the duplicate: the frame stays)
    g_spawned.push_back({actor, mod, kind, file});
    HLOG("%s spawned %s %s (%s) at %.1f %.1f %.1f", mod ? ModIdOf(mod) : "the bridge", kind == MML_ACTOR_CAR ? "a car" : "a person",
         Hex(reinterpret_cast<uintptr_t>(actor)).c_str(), file.c_str(), p.x, p.y, p.z);
    return actor;
}

bool WorldRemove(void *actor, std::string *err) {
    auto it = std::find_if(g_spawned.begin(), g_spawned.end(), [&](const Spawned &s) { return s.actor == actor; });
    if (it == g_spawned.end()) { *err = Hex(reinterpret_cast<uintptr_t>(actor)) + " was not made by world.spawn / actor_spawn (or is gone)"; return false; }
    if (!game::Game()) { *err = "no mission"; return false; }
    // A person in a car is the car's child (his frame in its frame): neither is taken out while one is in the other
    // (MafiaOnline: the game's removal does not get the people out first).
    int type = game::at<int>(actor, game::kActorType);
    if ((type == 2 || type == 27) && game::at<void *>(actor, game::kHumanVehicle)) { *err = "he sits in a car: get him out first"; return false; }
    if (type == game::kActorTypeCar) {
        std::vector<void *> all;
        game::ListActors(&all);
        for (void *a : all) {
            int t = game::at<int>(a, game::kActorType);
            if ((t == 2 || t == 27) && game::at<void *>(a, game::kHumanVehicle) == actor) {
                *err = std::string(t == 2 ? "the player" : "someone (" + Hex(reinterpret_cast<uintptr_t>(a)) + ")") + " sits in it";
                return false;
            }
        }
    }
    auto remove = [&] {
        if (E().hudCar && game::LastCar() == actor)   // the HUD's car (the speedometer's) would point at a freed actor
            game::at<void *>(reinterpret_cast<void *>(E().hud), E().hudCar) = nullptr;
        reinterpret_cast<ActorFn>(E().gameRemoveTemp)(game::Game(), nullptr, actor);
    };
    if (DWORD ex = Guarded(remove)) { *err = Faulted("removing it", ex); return false; }
    HLOG("removed %s (%s)", Hex(reinterpret_cast<uintptr_t>(actor)).c_str(), it->model.c_str());
    g_spawned.erase(it);
    g_removing.push_back(actor);
    return true;
}

bool WorldSeat(void *human, void *car, int seat, std::string *err) {
    if (!game::g_exe) { *err = "not a known build of the game"; return false; }
    int type = human ? game::at<int>(human, game::kActorType) : 0;
    if (type != 2 && type != 27) { *err = "not a person (the player, type 2, or a character, type 27)"; return false; }
    if (!car || game::at<int>(car, game::kActorType) != game::kActorTypeCar) { *err = "not a car"; return false; }
    if (seat < 0 || seat > 7) { *err = "seat: 0 the driver's, 1 beside him, 2 and 3 behind"; return false; }
    void *acts[2] = {human, car};
    char code[64];
    snprintf(code, sizeof code, "human_force_settocar 0, 1, %d", seat);
    return ScriptExec(code, nullptr, acts, 2, nullptr, 0, nullptr, nullptr, err);
}

bool WorldTeleport(void *actor, const MmlVec3 &pos, const MmlVec3 *dir, bool repair, std::string *err) {
    if (!game::g_exe || !E().progCtor) { *err = "moving actors is not known for this build"; return false; }
    actor = Reference(actor);
    void *frame = actor ? game::at<void *>(actor, game::kActorFrame) : nullptr;
    if (!frame) { *err = "the actor has no frame"; return false; }
    int type = game::at<int>(actor, game::kActorType);
    if (dir) {
        V3 d{dir->x, dir->y, dir->z};
        d = Flat(d);   // (a car turned about y too: its pitch and roll are the ground's, set by car_repair)
        auto turn = [&] {
            game::at<V3>(actor, game::kActorFacing) = d;
            reinterpret_cast<SetDirFn>(Vt(frame, kVtSetDir))(frame, &d, 0.0f);
            reinterpret_cast<UpdateFn>(Vt(frame, kVtUpdate))(frame);
        };
        if (DWORD ex = Guarded(turn)) { *err = Faulted("turning it", ex); return false; }
    }
    std::string code = "getactorframe 0, 0\nfrm_setpos 0, 0\nactor_setpos 0, 0";
    if (type == game::kActorTypeCar && repair) code += "\ncar_repair 0";
    float flts[3] = {pos.x, pos.y, pos.z};
    return ScriptExec(code.c_str(), nullptr, &actor, 1, flts, 3, nullptr, nullptr, err);
}

std::string WorldItemName(int id) {
    std::vector<ItemRec> items = Items();
    return id >= 0 && id < static_cast<int>(items.size()) ? items[static_cast<size_t>(id)].name : std::string();
}

int WorldItemId(const char *name, std::string *err) {
    std::vector<ItemRec> items = Items();
    if (items.empty()) { *err = "the items are not known for this build (or not loaded)"; return -1; }
    return Pick(items, name ? name : "", "item", [](const ItemRec &i) { return std::vector<std::string>{i.name}; }, err);
}

bool WorldGive(void *human, int item, int loaded, int reserve, std::string *err) {
    if (!game::g_exe) { *err = "not a known build of the game"; return false; }
    int type = human ? game::at<int>(human, game::kActorType) : 0;
    if (type != 2 && type != 27) { *err = "not a person (the player, type 2, or a character, type 27)"; return false; }
    std::vector<ItemRec> items = Items();
    if (item < 0 || item >= static_cast<int>(items.size())) { *err = "no item " + std::to_string(item) + " (world.items)"; return false; }
    if (loaded < 0) loaded = items[static_cast<size_t>(item)].magazine;
    if (reserve < 0) reserve = items[static_cast<size_t>(item)].reserve;
    char code[96];
    snprintf(code, sizeof code, "human_addweapon 0, %d, %d, %d", item, loaded, reserve);
    return ScriptExec(code, nullptr, &human, 1, nullptr, 0, nullptr, nullptr, err);
}

// An actor is being destroyed (its destructor's hook, before the game's). One that actor_remove took out leaves its
// model in the scene: the sector it is linked to keeps the frame (seen in the 1.2 lab: a removed person's model stood
// where he was), so the frame is unlinked here (I3D_frame::LinkTo(null), what takes a frame out of its parent);
// the actor's destructor then releases it (+0x6C).
void WorldActorGone(void *actor) {
    g_spawned.erase(std::remove_if(g_spawned.begin(), g_spawned.end(), [&](const Spawned &s) { return s.actor == actor; }), g_spawned.end());
    auto it = std::find(g_removing.begin(), g_removing.end(), actor);
    if (it == g_removing.end()) return;
    g_removing.erase(it);
    void *frame = game::at<void *>(actor, game::kActorFrame);
    if (!frame) return;
    auto unlink = [&] { reinterpret_cast<LinkToFn>(Vt(frame, kVtLinkTo))(frame, nullptr, 0); };
    if (DWORD ex = Guarded(unlink)) HWARN("world: unlinking a removed actor's model %s", Faulted("", ex).c_str());
}
void WorldMissionClose() {
    g_spawned.clear();
    g_removing.clear();
}

namespace {
V3 Vec(const json &v) { return V3{v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()}; }
json Arr(const V3 &v) { return json::array({std::round(v.x * 1000) / 1000, std::round(v.y * 1000) / 1000, std::round(v.z * 1000) / 1000}); }
bool IsVec(const json &v) { return v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number(); }

// A place for the bridge: [x, y, z], or an actor ("player", "car", an address) and `ahead` metres in front of it
// (the car for a player in one). fwd: that actor's forward (or +z for coordinates); fromActor: it was an actor.
bool Place(const json &p, const char *key, float aheadDefault, V3 *pos, V3 *fwd, bool *fromActor, std::string *err) {
    json at = p.contains(key) ? p[key] : json("player");
    *fromActor = !IsVec(at);
    if (!*fromActor) {
        *pos = Vec(at);
        *fwd = {0, 0, 1};
        return true;
    }
    if (!at.is_string()) { *err = std::string(key) + ": [x, y, z] or an actor (\"player\", \"car\", \"0x...\")"; return false; }
    void *a = nullptr;
    if (!ParseActor(at.get<std::string>(), &a, err)) { *err = std::string(key) + ": " + *err; return false; }
    a = Reference(a);
    *fwd = Forward(a);
    float ahead = p.value("ahead", aheadDefault);
    *pos = game::ActorWorldPos(a) + *fwd * ahead;
    return true;
}
// The ground under a place (the static world's), when there is some within 3 m above and 30 m below.
void Ground(V3 *pos, float lift) {
    V3 hit{}, n{};
    if (game::TestLine(*pos + V3{0, 3, 0}, *pos - V3{0, 30, 0}, &hit, &n)) pos->y = hit.y + lift;
}
json Describe(void *actor) {
    json j = {{"actor", Hex(reinterpret_cast<uintptr_t>(actor))}, {"type", game::at<int>(actor, game::kActorType)}};
    if (void *f = game::at<void *>(actor, game::kActorFrame)) j["name"] = Str(R32(reinterpret_cast<uintptr_t>(f) + game::kFrameName), 64);
    j["pos"] = Arr(game::ActorWorldPos(actor));
    j["facing"] = Arr(Forward(actor));
    return j;
}
} // namespace

bool WorldMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.compare(0, 6, "world.") != 0) return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "world.spawn") {
            // {car: name | number, color} | {human: model} | {model, kind: "car" | "human"}; at, ahead, dir, ground
            int kind = 0;
            std::string model;
            if (p.contains("car")) {
                kind = MML_ACTOR_CAR;
                const json &c = p["car"];
                std::vector<CarRec> cars = Cars();
                std::string want = c.is_number_integer() ? std::to_string(c.get<int>()) : c.is_string() ? c.get<std::string>() : "";
                int id = Pick(cars, want, "car", [](const CarRec &r) { return std::vector<std::string>{r.name, r.prefix}; }, &err);
                if (id < 0) { *reply = "car: " + err + " (world.cars lists them)"; return true; }
                int color = p.value("color", 0);
                if (color < 0 || color > 99) { *reply = "color: 0..99 (the files <prefix>00.i3d, <prefix>01.i3d...)"; return true; }
                char tail[8];
                snprintf(tail, sizeof tail, "%02d.i3d", color);
                for (const CarRec &r : cars) if (r.id == id) model = r.prefix + tail;
            } else if (p.contains("human")) {
                kind = MML_ACTOR_HUMAN;
                model = p.value("human", "");
            } else {
                model = p.value("model", "");
                std::string k = p.value("kind", "");
                kind = k == "car" ? MML_ACTOR_CAR : k == "human" ? MML_ACTOR_HUMAN : 0;
                if (!kind) { *reply = "car: a car's name or number, or human: a person's model (\"Paulie\"), or model with kind \"car\" | \"human\""; return true; }
            }
            V3 pos, fwd;
            bool fromActor = false;
            if (!Place(p, "at", kind == MML_ACTOR_CAR ? 8.0f : 2.5f, &pos, &fwd, &fromActor, &err)) { *reply = err; return true; }
            if (p.value("ground", true)) Ground(&pos, kind == MML_ACTOR_CAR ? 0.5f : 0.0f);
            V3 d = kind == MML_ACTOR_HUMAN && fromActor ? fwd * -1.0f : fwd;   // a person faces who he was put before
            if (p.contains("dir")) {
                if (!IsVec(p["dir"])) { *reply = "dir: [x, y, z]"; return true; }
                d = Vec(p["dir"]);
            }
            void *car = nullptr;   // a person put into a car's seat at once
            if (p.contains("in")) {
                if (kind != MML_ACTOR_HUMAN || !p["in"].is_string() || !ParseActor(p["in"].get<std::string>(), &car, &err)) {
                    *reply = "in: a car (\"car\", \"0x...\") for a person" + (err.empty() ? "" : ": " + err);
                    return true;
                }
                car = Reference(car);
            }
            MmlVec3 mp{pos.x, pos.y, pos.z}, md{d.x, d.y, d.z};
            void *a = WorldSpawn(nullptr, kind, model.c_str(), &mp, &md, &err);
            if (!a) { *reply = err; return true; }
            if (car && !WorldSeat(a, car, p.value("seat", 1), &err)) { *reply = "spawned " + Hex(reinterpret_cast<uintptr_t>(a)) + ", not seated: " + err; return true; }
            json j = Describe(a);
            j["model"] = model.find('.') == std::string::npos ? model + ".i3d" : model;
            *reply = dump(j);
        } else if (method == "world.seat") {
            // {actor: a person, car (default "car"), seat (default 1)}
            void *a = nullptr, *car = nullptr;
            if (!ParseActor(p.value("actor", ""), &a, &err)) { *reply = "actor: " + err; return true; }
            if (!ParseActor(p.value("car", "car"), &car, &err)) { *reply = "car: " + err; return true; }
            if (!WorldSeat(a, Reference(car), p.value("seat", 1), &err)) { *reply = err; return true; }
            *reply = dump(Describe(a));
        } else if (method == "world.remove") {
            void *a = nullptr;
            if (!ParseActor(p.value("actor", ""), &a, &err) || !WorldRemove(a, &err)) { *reply = "actor: " + err; return true; }
            *reply = dump({{"removed", Hex(reinterpret_cast<uintptr_t>(a))}});
        } else if (method == "world.teleport") {
            // {actor (default "player"), to: [x, y, z] | an actor, ahead, dir, repair}
            void *a = nullptr;
            if (!ParseActor(p.value("actor", "player"), &a, &err)) { *reply = "actor: " + err; return true; }
            if (!p.contains("to")) { *reply = "to: [x, y, z] or an actor (\"car\", \"0x...\")"; return true; }
            V3 pos, fwd;
            bool fromActor = false;
            if (!Place(p, "to", 3.0f, &pos, &fwd, &fromActor, &err)) { *reply = err; return true; }
            if (fromActor && p.value("ground", true)) Ground(&pos, 0.0f);
            MmlVec3 mp{pos.x, pos.y, pos.z}, md{};
            bool turn = p.contains("dir");
            if (turn) {
                if (!IsVec(p["dir"])) { *reply = "dir: [x, y, z]"; return true; }
                V3 d = Vec(p["dir"]);
                md = {d.x, d.y, d.z};
            }
            if (!WorldTeleport(a, mp, turn ? &md : nullptr, p.value("repair", true), &err)) { *reply = err; return true; }
            *reply = dump(Describe(Reference(a)));
        } else if (method == "world.give") {
            // {actor (default "player"), item: name | number, loaded, reserve}
            void *a = nullptr;
            if (!ParseActor(p.value("actor", "player"), &a, &err)) { *reply = "actor: " + err; return true; }
            const json &w = p.contains("item") ? p["item"] : p.contains("weapon") ? p["weapon"] : json();
            std::string want = w.is_number_integer() ? std::to_string(w.get<int>()) : w.is_string() ? w.get<std::string>() : "";
            int id = WorldItemId(want.c_str(), &err);
            if (id < 0) { *reply = "item: " + err + " (world.items lists them)"; return true; }
            if (!WorldGive(a, id, p.value("loaded", -1), p.value("reserve", -1), &err)) { *reply = err; return true; }
            ItemRec it = Items()[static_cast<size_t>(id)];
            *reply = dump({{"actor", Hex(reinterpret_cast<uintptr_t>(a))}, {"item", id}, {"name", it.name},
                           {"loaded", p.value("loaded", -1) < 0 ? it.magazine : p.value("loaded", -1)},
                           {"reserve", p.value("reserve", -1) < 0 ? it.reserve : p.value("reserve", -1)}});
        } else if (method == "world.cars") {
            std::string q = Lower(p.value("query", ""));
            json list = json::array();
            for (const CarRec &c : Cars())
                if (q.empty() || Lower(c.name).find(q) != std::string::npos || Lower(c.prefix).find(q) != std::string::npos)
                    list.push_back({{"id", c.id}, {"name", c.name}, {"model", c.prefix + "00.i3d"}});
            *reply = dump({{"count", list.size()}, {"cars", list},
                           {"note", "world.spawn {car: name or id, color}: color picks the file (<prefix>01.i3d...); not every car has more than 00"}});
        } else if (method == "world.items") {
            std::string q = Lower(p.value("query", ""));
            json list = json::array();
            for (const ItemRec &i : Items())
                if (q.empty() || Lower(i.name).find(q) != std::string::npos)
                    list.push_back({{"id", i.id}, {"name", i.name}, {"kind", ItemKind(i.flags)}, {"model", i.model},
                                    {"magazine", i.magazine}, {"reserve", i.reserve}, {"flags", Hex(i.flags)}});
            *reply = dump({{"count", list.size()}, {"items", list}});
        } else if (method == "world.spawned") {
            json list = json::array();
            for (const Spawned &s : g_spawned) {
                json j = Describe(s.actor);
                j["by"] = s.mod ? ModIdOf(s.mod) : "bridge";
                j["model"] = s.model;
                list.push_back(j);
            }
            *reply = dump({{"known", SpawnKnown()}, {"spawned", list}});
        } else {
            *reply = "unknown method '" + method + "': world.spawn, world.remove, world.teleport, world.seat, world.give, world.cars, world.items, world.spawned";
            return true;
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
