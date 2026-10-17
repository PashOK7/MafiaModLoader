// Interaction (mml.h zone_add, use_point_add, timer_add and their removes; docs/MODDING.md "Interaction").
//
// Zones: spheres in the world (or following a frame) the player enters and leaves, on foot or in his car (his
// model's world position: in a car it is the car's); the mod's function is told of each crossing, the bridge's
// zones go to events.since (ZONE_ENTERED, ZONE_LEFT, with the actor). zone_watch makes a zone watch more than the
// player: every person the loader knows (humans.cpp), every car (the game's actors of type 4), or one actor, each
// one's crossings told with it; an actor destroyed inside leaves it without a crossing.
// Use-points: the game's own using objects, what the player's Action finds on foot (doors, the cars' seats, a body,
// a weapon on the ground: game_build.h uoAdd). A use-point is a record of the loader's in the game's registry
// (C_game + 0x29EC), its action's id a text id of the loader's in the game's text database (the label): the game
// finds it as it finds a door, sorts it with the others by distance and, where there are several, shows its own
// choice menu ("1 - Get in the car as the driver", "2 - Fish here"). The game's executor passes over an action it
// does not know, so the game does nothing for it and nothing else is taken from the player (the car beside it is
// still entered from the same menu). The pick is read where the game makes it: FindNearObjects' list kept (its
// hook) and ExecuteMenu's result (0xA9 + the item's index) mapped to it.
// Timers: the mission's clock (MML_EVENT_MISSION_TICK's time: they stand while the game is paused or loads), once
// or repeated. A mod's go with its unload, every zone, use-point and timer with the mission; a use-point on a frame
// of an actor that is destroyed goes with it.
//
// Bridge methods (game thread): interact.list, zone.add {at | on, bone, pos, radius}, use.add {..., label},
// timer.add {ms, repeat}, interact.remove {id | all}.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

// S_using_object (game_build.h uoAdd): what the registry points to; the S_GameItem at +0x28 is what the menu lists.
#pragma pack(push, 1)
struct UoRecord {
    float pos[3];             // in `matrix`'s space, or the world's
    float range2;             // horizontal reach, squared
    const float *matrix;      // a world matrix (a frame's), or null
    uint32_t flags;           // 0: the record and no frame are the game's to free
    void *frame, *actor, *owner;
    uint8_t transient, pad[3];
    uint32_t type;            // 1: an action
    uint32_t id;              // the action: its label's text id
    uint32_t zero;
    UoRecord *self;
};
#pragma pack(pop)
static_assert(sizeof(UoRecord) == 0x38, "S_using_object");
constexpr int kUsingObjects = 0x29EC;
constexpr int kMenuPicked = 0xA9;

enum Kind { kZone, kUse, kTimer };
struct Item {
    int id;
    Kind kind;
    MmlMod *mod;
    void *frame;              // followed (null: the world)
    V3 pos;                   // the world's, or in the frame's space
    float radius;
    std::string label;        // a use-point's (UTF-8)
    std::shared_ptr<UoRecord> rec;   // a use-point's record, registered
    MmlZoneFn zoneFn;
    MmlUseFn useFn;
    MmlTimerFn timerFn;
    void *user;
    bool inside;
    uint32_t who = MML_ZONE_PLAYER;   // a zone's (zone_watch)
    void *watched;            // MML_ZONE_ACTOR's actor
    std::vector<void *> in;   // a zone's actors inside (other than for the player alone)
    int ms, left;             // a timer's period and what is left of it (mission ms)
    bool repeat;
    int fired;
};
std::vector<Item> g_items;
int g_nextId;

using UoFn = int(__fastcall *)(void *registry, void *edx, void *record);
using FindFn = void(__fastcall *)(void *registry, void *edx, const float *at, const float *dir, void *out, void *frame);
using ExecFn = int(__fastcall *)(void *menu, int tick, int unused);
FindFn oFind;
ExecFn oExec;
bool g_hooked;
std::string g_hookWhy = "not asked for";
struct Found { uint32_t type, id; void *rec; };
std::vector<Found> g_found;   // the last FindNearObjects' list, until the menu that shows it returns
bool g_armed;
unsigned g_menus, g_picked;

const game::ExeBuild &E() { return game::Exe(); }

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

void *Registry() {
    void *g = game::g_exe ? game::Game() : nullptr;
    return g ? static_cast<uint8_t *>(g) + kUsingObjects : nullptr;
}

bool PlayerPlace(V3 *out, bool *onFoot) {
    void *pl = game::g_exe ? game::Player() : nullptr;
    void *model = pl ? ActorFrame(pl) : nullptr;
    float w[16];
    if (!model || !FrameWorld(model, w)) return false;
    *out = {w[12], w[13], w[14]};
    bool foot = true;
    auto g = [&] { foot = game::at<void *>(pl, game::kHumanVehicle) == nullptr; };
    Guarded(g);
    *onFoot = foot;
    return true;
}

bool Centre(const Item &it, V3 *out) {
    if (!it.frame) { *out = it.pos; return true; }
    float w[16];
    if (!FrameWorld(it.frame, w)) return false;
    *out = {w[12] + it.pos.x * w[0] + it.pos.y * w[4] + it.pos.z * w[8], w[13] + it.pos.x * w[1] + it.pos.y * w[5] + it.pos.z * w[9],
            w[14] + it.pos.x * w[2] + it.pos.y * w[6] + it.pos.z * w[10]};
    return true;
}

struct ZoneCall { Item *it; int inside; void *actor; };
void CallZone(void *p) { auto *c = static_cast<ZoneCall *>(p); c->it->zoneFn(c->it->id, c->inside, c->actor, c->it->user); }
struct UseCall { Item *it; };
void CallUse(void *p) { auto *c = static_cast<UseCall *>(p); c->it->useFn(c->it->id, c->it->user); }
void CallTimer(void *p) { auto *c = static_cast<UseCall *>(p); c->it->timerFn(c->it->id, c->it->user); }

void Tell(Item &it, const char *type, void *actor = nullptr) {
    json d = {{"id", it.id}, {"by", it.mod ? ModIdOf(it.mod) : "bridge"}};
    if (it.kind == kUse) d["label"] = it.label;
    if (actor) {
        char b[16];
        snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(actor)));
        d["actor"] = b;
    }
    EventsRecord(type, d.dump(-1, ' ', false, json::error_handler_t::replace));
}

// An actor's model's world position (in a car: the car's place for its crew, as for the player).
bool ActorPlace(void *actor, V3 *out) {
    void *model = actor ? ActorFrame(actor) : nullptr;
    float w[16];
    if (!model || !FrameWorld(model, w)) return false;
    *out = {w[12], w[13], w[14]};
    return true;
}
int TypeOf(void *a) {
    int t = 0;
    auto f = [&] { t = game::at<int>(a, game::kActorType); };
    Guarded(f);
    return t;
}

void Unregister(Item &it) {
    if (!it.rec) return;
    void *reg = Registry();
    auto f = [&] { if (reg) reinterpret_cast<UoFn>(E().uoDel)(reg, nullptr, it.rec.get()); };
    Guarded(f);
    it.rec.reset();
}

void __fastcall hkFind(void *registry, void *edx, const float *at, const float *dir, void *out, void *frame) {
    oFind(registry, edx, at, dir, out, frame);
    g_found.clear();
    g_armed = false;
    auto f = [&] {
        const uint32_t *b = static_cast<uint32_t **>(out)[1], *e = static_cast<uint32_t **>(out)[2];
        for (const uint32_t *p = b; p && p < e && g_found.size() < 64; p += 4) g_found.push_back({p[0], p[1], reinterpret_cast<void *>(p[3])});
    };
    Guarded(f);
    g_armed = !g_found.empty();   // (the action menu follows at once when anything was found)
}

int __fastcall hkExec(void *menu, int tick, int unused) {
    if (!g_armed) return oExec(menu, tick, unused);
    std::vector<Found> found;
    found.swap(g_found);
    g_armed = false;
    g_menus++;
    int r = oExec(menu, tick, unused);
    int idx = r - kMenuPicked;
    if (idx >= 0 && idx < static_cast<int>(found.size()) && found[idx].type == 1)
        for (Item &it : g_items)
            if (it.kind == kUse && it.rec && it.rec.get() == found[idx].rec) {
                g_picked++;
                Tell(it, "USED");
                if (it.useFn && (!it.mod || ModAlive(it.mod))) {
                    UseCall uc{&it};
                    ModCall(it.mod, CallUse, &uc, "its use-point function");
                }
                break;
            }
    return r;
}

bool EnsureHooks(std::string *err) {
    if (g_hooked) return true;
    if (!game::g_exe || !E().uoAdd || !E().uoDel || !E().uoFind || !E().menuExecute) { *err = g_hookWhy = "the using objects are not known for this build"; return false; }
    if (!HookAdd(nullptr, E().uoFind, reinterpret_cast<void *>(&hkFind), reinterpret_cast<void **>(&oFind), err) ||
        !HookAdd(nullptr, E().menuExecute, reinterpret_cast<void *>(&hkExec), reinterpret_cast<void **>(&oExec), err)) {
        g_hookWhy = *err;
        return false;
    }
    g_hooked = true;
    g_hookWhy = "hooked";
    return true;
}

} // namespace

int HudHelpLine(const std::string &utf8) {
    if (!game::g_exe || !E().hudHelpAdd || !E().hud) return 0;
    uint32_t id = GameTextId(utf8);
    if (!id) return 0;
    using HelpFn = int(__fastcall *)(uintptr_t hud, void *edx, uint32_t id);
    int ms = 0;
    auto f = [&] { ms = reinterpret_cast<HelpFn>(E().hudHelpAdd)(E().hud, nullptr, id); };
    if (Guarded(f)) return 0;
    return ms;
}

int ZoneAdd(MmlMod *mod, const float *pos, float radius, void *frame, MmlZoneFn fn, void *user, std::string *err) {
    if (!game::g_exe) { *err = "no game"; return 0; }
    if (!(radius > 0)) { *err = "radius: more than 0"; return 0; }
    Item it{};
    it.id = ++g_nextId;
    it.kind = kZone;
    it.mod = mod;
    it.frame = frame;
    it.pos = pos ? V3{pos[0], pos[1], pos[2]} : V3{0, 0, 0};
    it.radius = radius;
    it.zoneFn = fn;
    it.user = user;
    g_items.push_back(it);
    return it.id;
}

int UseAdd(MmlMod *mod, const float *pos, float radius, void *frame, const char *label, MmlUseFn fn, void *user, std::string *err) {
    if (!game::g_exe) { *err = "no game"; return 0; }
    if (!(radius > 0)) { *err = "radius: more than 0"; return 0; }
    if (!label || !*label) { *err = "label: the menu's line for it (\"Fish here\")"; return 0; }
    void *reg = Registry();
    if (!reg) { *err = "no mission (the using objects are the mission's)"; return 0; }
    if (!EnsureHooks(err)) return 0;
    uint32_t text = GameTextId(label);
    if (!text) { *err = "the game's text database is not hooked (menus.cpp)"; return 0; }
    Item it{};
    it.id = ++g_nextId;
    it.kind = kUse;
    it.mod = mod;
    it.frame = frame;
    it.pos = pos ? V3{pos[0], pos[1], pos[2]} : V3{0, 0, 0};
    it.radius = radius;
    it.label = label;
    it.useFn = fn;
    it.user = user;
    it.rec = std::make_shared<UoRecord>();
    UoRecord &r = *it.rec;
    memset(&r, 0, sizeof r);
    r.pos[0] = it.pos.x; r.pos[1] = it.pos.y; r.pos[2] = it.pos.z;
    r.range2 = radius * radius;
    if (frame) {
        const float *w = nullptr;
        auto g = [&] { w = game::WorldMatrix(frame); };
        if (Guarded(g) || !w) { *err = "frame: not a frame"; return 0; }
        r.matrix = w;   // (the frame's own world matrix: the record follows it)
    }
    r.type = 1;
    r.id = text;
    r.self = &r;
    auto f = [&] { reinterpret_cast<UoFn>(E().uoAdd)(reg, nullptr, &r); };
    if (Guarded(f)) { *err = "AddObject faulted"; return 0; }
    g_items.push_back(it);
    return it.id;
}

int TimerAdd(MmlMod *mod, int ms, bool repeat, MmlTimerFn fn, void *user, std::string *err) {
    if (ms <= 0) { *err = "ms: more than 0"; return 0; }
    Item it{};
    it.id = ++g_nextId;
    it.kind = kTimer;
    it.mod = mod;
    it.ms = it.left = ms;
    it.repeat = repeat;
    it.timerFn = fn;
    it.user = user;
    g_items.push_back(it);
    return it.id;
}

bool ZoneWatch(MmlMod *mod, int id, uint32_t who, void *actor, std::string *err) {
    auto it = std::find_if(g_items.begin(), g_items.end(), [&](const Item &i) { return i.id == id && i.kind == kZone && (!mod || i.mod == mod); });
    if (it == g_items.end()) { *err = "no such zone of this mod's"; return false; }
    who &= MML_ZONE_PLAYER | MML_ZONE_PEOPLE | MML_ZONE_CARS | MML_ZONE_ACTOR;
    if (!who) { *err = "who: MML_ZONE_PLAYER, _PEOPLE, _CARS, _ACTOR"; return false; }
    if ((who & MML_ZONE_ACTOR) && !actor) { *err = "MML_ZONE_ACTOR: an actor"; return false; }
    bool wasPlayerOnly = it->who == MML_ZONE_PLAYER;
    it->who = who;
    it->watched = who & MML_ZONE_ACTOR ? actor : nullptr;
    if (wasPlayerOnly && it->inside && who != MML_ZONE_PLAYER) {   // (the player inside: kept as inside, no crossing)
        if (void *p = game::Player()) it->in.assign(1, p);
    }
    if (who == MML_ZONE_PLAYER) {
        void *p = game::Player();
        it->inside = p && std::find(it->in.begin(), it->in.end(), p) != it->in.end();
        it->in.clear();
    }
    return true;
}

bool InteractRemove(MmlMod *mod, int id) {
    auto it = std::find_if(g_items.begin(), g_items.end(), [&](const Item &i) { return i.id == id && (!mod || i.mod == mod); });
    if (it == g_items.end()) return false;
    Unregister(*it);
    g_items.erase(it);
    return true;
}

// Every frame of a loaded mission: the zones' crossings (and the use-points' "inside", for interact.list).
void InteractFrame() {
    if (g_items.empty()) return;
    V3 p;
    bool foot = false;
    bool have = PlayerPlace(&p, &foot);
    // who the zones that watch more than the player watch: read once a frame, when one asks
    std::vector<void *> people, cars;
    bool listed = false;
    auto list = [&] {
        if (listed) return;
        listed = true;
        void *buf[1024];
        int n = std::min(HumansList(buf, 1024), 1024);
        people.assign(buf, buf + n);
        std::vector<void *> all;
        auto f = [&] { game::ListActors(&all); };
        Guarded(f);
        for (void *a : all) if (TypeOf(a) == game::kActorTypeCar) cars.push_back(a);
    };
    for (size_t i = 0; i < g_items.size(); i++) {
        Item &it = g_items[i];
        if (it.kind == kTimer) continue;
        if (it.kind == kZone && it.who != MML_ZONE_PLAYER) {
            V3 c;
            if (!Centre(it, &c)) continue;
            std::vector<void *> now;
            auto consider = [&](void *a) {
                V3 q;
                if (a && std::find(now.begin(), now.end(), a) == now.end() && ActorPlace(a, &q) && Len(q - c) <= it.radius) now.push_back(a);
            };
            if (it.who & (MML_ZONE_PEOPLE | MML_ZONE_CARS)) list();
            if (it.who & MML_ZONE_PLAYER) consider(game::Player());
            if (it.who & MML_ZONE_PEOPLE) for (void *a : people) consider(a);
            if (it.who & MML_ZONE_CARS) for (void *a : cars) consider(a);
            if (it.who & MML_ZONE_ACTOR) consider(it.watched);
            int id = it.id;
            std::vector<std::pair<void *, int>> crossings;
            for (void *a : now) if (std::find(it.in.begin(), it.in.end(), a) == it.in.end()) crossings.push_back({a, 1});
            for (void *a : it.in) if (std::find(now.begin(), now.end(), a) == now.end()) crossings.push_back({a, 0});
            it.in = now;
            it.inside = !now.empty();
            for (const auto &x : crossings) {
                auto self = std::find_if(g_items.begin(), g_items.end(), [&](const Item &z) { return z.id == id; });
                if (self == g_items.end()) break;   // (removed by its own function)
                Tell(*self, x.second ? "ZONE_ENTERED" : "ZONE_LEFT", x.first);
                if (self->zoneFn && (!self->mod || ModAlive(self->mod))) {
                    ZoneCall zc{&*self, x.second, x.first};
                    ModCall(self->mod, CallZone, &zc, "its zone function");
                }
            }
            if (i >= g_items.size() || g_items[i].id != id) i--;
            continue;
        }
        V3 c;
        bool in = have && Centre(it, &c) && Len(p - c) <= it.radius && (it.kind == kZone || foot);
        if (in == it.inside) continue;
        it.inside = in;
        if (it.kind != kZone) continue;
        int id = it.id;
        Tell(it, in ? "ZONE_ENTERED" : "ZONE_LEFT", game::Player());
        if (it.zoneFn && (!it.mod || ModAlive(it.mod))) {
            ZoneCall zc{&it, in ? 1 : 0, game::Player()};
            ModCall(it.mod, CallZone, &zc, "its zone function");
        }
        if (i >= g_items.size() || g_items[i].id != id) i--;   // (removed by its own function)
    }
}

// The mission's tick (its time: the pause and the loading stand still).
void InteractTick(int ms) {
    for (size_t i = 0; i < g_items.size();) {
        Item &it = g_items[i];
        if (it.kind != kTimer) { i++; continue; }
        it.left -= ms;
        if (it.left > 0) { i++; continue; }
        it.fired++;
        int id = it.id;
        Tell(it, "TIMER");
        if (it.timerFn && (!it.mod || ModAlive(it.mod))) {
            UseCall uc{&it};
            ModCall(it.mod, CallTimer, &uc, "its timer function");
        }
        auto again = std::find_if(g_items.begin(), g_items.end(), [&](const Item &x) { return x.id == id; });
        if (again == g_items.end()) continue;   // (removed by its own function)
        i = static_cast<size_t>(again - g_items.begin());
        if (again->repeat) { again->left += again->ms; if (again->left <= 0) again->left = again->ms; i++; }
        else g_items.erase(again);
    }
}

void InteractModGone(MmlMod *mod) {
    for (size_t i = 0; i < g_items.size();)
        if (g_items[i].mod == mod) { Unregister(g_items[i]); g_items.erase(g_items.begin() + i); }
        else i++;
}
void InteractMissionClose() {   // (before the game's: its registry is still there)
    for (Item &it : g_items) Unregister(it);
    g_items.clear();
    g_found.clear();
    g_armed = false;
}
// An actor's destructor (before the game's): what follows a frame of its model goes (a use-point's record would
// read the frame's matrix after it is freed).
void InteractActorGone(void *actor) {
    for (Item &it : g_items) {   // (gone from the zones it was in without a crossing; a zone watching it alone stops)
        if (it.kind != kZone) continue;
        it.in.erase(std::remove(it.in.begin(), it.in.end(), actor), it.in.end());
        if (it.watched == actor) { it.watched = nullptr; it.who &= ~MML_ZONE_ACTOR; }
    }
    void *model = actor ? game::at<void *>(actor, game::kActorFrame) : nullptr;
    if (!model) return;
    for (size_t i = 0; i < g_items.size();) {
        bool on = false;
        for (void *f = g_items[i].frame; f && !on; f = game::at<void *>(f, game::kFrameParent)) on = f == model;
        if (on) { Unregister(g_items[i]); g_items.erase(g_items.begin() + i); }
        else i++;
    }
}

bool InteractMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    static const char *kMethods[] = {"interact.list", "zone.add", "use.add", "timer.add", "interact.remove"};
    if (std::none_of(std::begin(kMethods), std::end(kMethods), [&](const char *m) { return method == m; })) return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    auto r3 = [](const V3 &v) { return json::array({std::round(v.x * 100) / 100, std::round(v.y * 100) / 100, std::round(v.z * 100) / 100}); };
    std::string err;
    try {
        if (method == "interact.list") {
            json list = json::array();
            for (const Item &it : g_items) {
                json j = {{"id", it.id}, {"kind", it.kind == kZone ? "zone" : it.kind == kUse ? "use" : "timer"}, {"by", it.mod ? ModIdOf(it.mod) : "bridge"}};
                if (it.kind == kTimer) { j["ms"] = it.ms; j["left"] = it.left; j["repeat"] = it.repeat; j["fired"] = it.fired; }
                else {
                    V3 c;
                    if (Centre(it, &c)) j["at"] = r3(c);
                    j["radius"] = it.radius;
                    j["inside"] = it.inside;
                    if (it.kind == kZone && it.who != MML_ZONE_PLAYER) {
                        json ins = json::array();
                        for (void *a : it.in) {
                            char b[16];
                            snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(a)));
                            ins.push_back(b);
                        }
                        j["who"] = it.who;
                        j["in"] = ins;
                    }
                    if (it.kind == kUse) { j["label"] = it.label; j["text_id"] = it.rec ? it.rec->id : 0; }
                }
                list.push_back(j);
            }
            V3 pl;
            bool foot = false;
            json out = {{"items", list}, {"using_objects", g_hookWhy}, {"action_menus", g_menus}, {"picked", g_picked}};
            if (PlayerPlace(&pl, &foot)) { out["player"] = r3(pl); out["on_foot"] = foot; }
            *reply = dump(out);
            *ok = true;
            return true;
        }
        if (method == "interact.remove") {
            if (p.value("all", false)) {
                size_t n = 0;
                for (size_t i = 0; i < g_items.size();)
                    if (!g_items[i].mod) { Unregister(g_items[i]); g_items.erase(g_items.begin() + i); n++; }
                    else i++;
                *reply = dump({{"removed", n}});
            } else {
                if (!InteractRemove(nullptr, p.value("id", 0))) { *reply = "interact.remove: id: one of the bridge's (interact.list)"; return true; }
                *reply = dump({{"removed", 1}});
            }
            *ok = true;
            return true;
        }
        if (method == "timer.add") {
            int id = TimerAdd(nullptr, p.value("ms", 0), p.value("repeat", false), nullptr, nullptr, &err);
            if (!id) { *reply = "timer.add: " + err; return true; }
            *reply = dump({{"id", id}});
            *ok = true;
            return true;
        }
        // zone.add, use.add: a place (`at`, or `on` an actor with `bone` and `pos`; default: where the player is)
        void *frame = nullptr;
        V3 pos{0, 0, 0};
        if (p.contains("on") && p["on"].is_string()) {
            void *actor = nullptr;
            if (!ParseActor(p["on"].get<std::string>(), &actor, &err)) { *reply = method + ": on: " + err; return true; }
            frame = ActorFrame(actor);
            if (!frame) { *reply = method + ": on: the actor has no model"; return true; }
            if (p.contains("bone") && p["bone"].is_string()) {
                void *b = game::FindChild(frame, p["bone"].get<std::string>().c_str());
                if (!b) { *reply = method + ": bone: not in its model"; return true; }
                frame = b;
            }
            if (p.contains("pos") && p["pos"].is_array() && p["pos"].size() == 3) pos = {p["pos"][0].get<float>(), p["pos"][1].get<float>(), p["pos"][2].get<float>()};
        } else if (p.contains("at") && p["at"].is_array() && p["at"].size() == 3) {
            pos = {p["at"][0].get<float>(), p["at"][1].get<float>(), p["at"][2].get<float>()};
        } else {
            bool foot;
            if (!PlayerPlace(&pos, &foot)) { *reply = method + ": at: [x, y, z] (or no player to put it at)"; return true; }
        }
        const float f[3] = {pos.x, pos.y, pos.z};
        int id = method == "zone.add" ? ZoneAdd(nullptr, f, p.value("radius", 3.0f), frame, nullptr, nullptr, &err)
                                      : UseAdd(nullptr, f, p.value("radius", 2.0f), frame, p.value("label", "").c_str(), nullptr, nullptr, &err);
        if (!id) { *reply = method + ": " + err; return true; }
        // zone.add's `who`: "player" (default), "people", "cars", "all" (the three), or an actor ("car", "0x...")
        std::string who = method == "zone.add" ? p.value("who", "player") : "player";
        if (who != "player") {
            uint32_t w = who == "people" ? MML_ZONE_PEOPLE : who == "cars" ? MML_ZONE_CARS : who == "all" ? MML_ZONE_PEOPLE | MML_ZONE_CARS : MML_ZONE_ACTOR;
            void *actor = nullptr;
            if (w == MML_ZONE_ACTOR && !ParseActor(who, &actor, &err)) { InteractRemove(nullptr, id); *reply = "who: " + err; return true; }
            if (!ZoneWatch(nullptr, id, w, actor, &err)) { InteractRemove(nullptr, id); *reply = "who: " + err; return true; }
        }
        *reply = dump({{"id", id}, {"at", r3(pos)}, {"who", who}});
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
