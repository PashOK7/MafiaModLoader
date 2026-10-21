// What strikes people, before the game's reaction (mml.h hit_filter, human_hit; docs/MODDING.md "Hits before the
// game").
//
// Every hit a person takes passes the mods' filters before the game reacts to it: C_human::Hit (game_hooks.cpp
// hkHumanHit: bullets, explosions, cars, falls, the scripts), the traffic pedestrians' Hit (anims.cpp hkPedHit: pedHit,
// thiscall(element, type, dir, part, attacker, float damage): the traffic generator's fire and explosions, pedCarHit,
// the bullets), and before that, for a bullet, the pedestrian system's own Hit (game_build.h pedBulletHit, the vtable
// +0x7C of every pedestrian's collision object's owner, thiscall(system, type, dir, from, pos, float damage, attacker,
// ?, model frame) ret 0x20): it finds the element by the model struck and wakes it (pedWake: a dead one would cry out at
// its next tick) before it calls pedHit twice, so the bullet is filtered there, once, and pedHit's own filter is passed
// by while it runs. The filters run in the order they were set; each sees the hit (MmlHumanHit) and may change its
// type, dir, pos, damage, attacker and part, which the game then gets; the first that returns MML_HIT_TAKEN ends it:
// the game's Hit does not run (a corpse's hit is the mod's own), and HUMAN_HIT or PEDESTRIAN_HIT tells it with `taken`.
// A filter that faults counts as MML_HIT_GAME. A hit a filter's own code makes (human_hit inside the filter) passes the
// filters by.
//
// human_hit calls the game's own Hit: a C_human's through his vtable +0x7C (C_human::Hit in both builds, a player's
// override first), a pedestrian's pedHit. mafia-ragdoll's knock-down by a body is the game's car hit with no car and no
// damage: a C_human's type 8 with the relative velocity as dir (over 3 m/s: he falls), a pedestrian's type 5 with a
// strength (the damage) over 16 (0x624078 in 1.2: the full knockdown; no car: no health taken).
//
// The bridge has a filter of its own, after the mods': hits.take takes the hits of a person (or everyone's) for a time,
// so an agent tests what a mod's filter would do without one.
//
// Bridge methods (game thread): human.hit {actor | pedestrian, type, dir, pos, damage, attacker, part}, hits.take
// {actor | pedestrian | all, ms, clear}, hits.state.
#include "host.h"
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kVtHit = 0x7C;

const game::ExeBuild &E() { return game::Exe(); }

template <class F> bool Guarded(F &f) {
    __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Filter { MmlMod *mod; MmlHitFilterFn fn; void *user; };
std::vector<Filter> g_filters;
// the bridge's own filter (hits.take): an agent's test takes the hits of one person (null: everyone) for a time
struct Take { void *who; DWORD until; unsigned taken; };
std::vector<Take> g_takes;
int g_inFilter;              // a filter's own code runs (its human_hit passes the filters by)
void *g_inBullet;            // the element pedBulletHit filtered, while the game's code under it runs
unsigned g_seen, g_taken, g_faults, g_called;
bool g_bulletHooked;
std::string g_bulletWhy = "not tried";

using PedBulletFn = int(__fastcall *)(void *sys, void *edx, int type, V3 *dir, V3 *from, V3 *pos, float damage, void *attacker,
                                      int a7, void *frame);
PedBulletFn oPedBullet;

struct FilterCall { const Filter *f; void *person; int ped; MmlHumanHit *hit; int r; };
void CallFilter(void *p) {
    auto *c = static_cast<FilterCall *>(p);
    c->r = c->f->fn(c->person, c->ped, c->hit, c->f->user);
}

int __fastcall hkPedBullet(void *sys, void *edx, int type, V3 *dir, V3 *from, V3 *pos, float damage, void *attacker, int a7,
                           void *frame) {
    void *element = (g_filters.empty() && g_takes.empty()) || g_inFilter ? nullptr : PedestrianOfFrame(frame);
    if (!element) return oPedBullet(sys, edx, type, dir, from, pos, damage, attacker, a7, frame);
    MmlHumanHit h{};
    h.type = type;
    if (dir) h.dir = {dir->x, dir->y, dir->z};
    if (pos) h.pos = {pos->x, pos->y, pos->z};
    h.damage = damage;
    h.attacker = attacker;
    h.was_alive = h.alive = PedestrianHealth(element) > 0;
    if (HitsFilter(element, 1, &h)) {
        MmlEvent ev{};
        ev.size = sizeof ev;
        ev.type = MML_EVENT_PEDESTRIAN_HIT;
        ev.actor = element;
        ev.other = frame;
        ev.hit = &h;
        Dispatch(ev);
        return 1;
    }
    V3 d{h.dir.x, h.dir.y, h.dir.z}, at{h.pos.x, h.pos.y, h.pos.z};
    g_inBullet = element;
    int r = oPedBullet(sys, edx, h.type, dir ? &d : nullptr, from, pos ? &at : nullptr, h.damage, h.attacker, a7, frame);
    g_inBullet = nullptr;
    return r;
}

std::string Hex(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

bool AliveFlag(void *human) {
    uint8_t a = 0;
    auto f = [&] { a = game::at<uint8_t>(human, game::kActorAlive); };
    Guarded(f);
    return a != 0;
}

// whom a bridge call names: `pedestrian` (an element's address: not an actor, no vtable of the game's), else `actor`
bool Person(const json &p, void **out, std::string *err) {
    *out = nullptr;
    if (p.contains("pedestrian")) {
        std::string a = p.value("pedestrian", "");
        char *end = nullptr;
        uintptr_t v = static_cast<uintptr_t>(strtoul(a.c_str(), &end, 16));
        if (a.empty() || (end && *end)) { *err = "pedestrian: an element's address (pedestrians.list)"; return false; }
        if (!PedestrianKnown(reinterpret_cast<void *>(v))) { *err = "pedestrian: " + a + " is not a traffic pedestrian (pedestrians.list)"; return false; }
        *out = reinterpret_cast<void *>(v);
        return true;
    }
    std::string a = p.value("actor", "player");
    if (a == "player") *out = HumanPlayer();
    else if (!ParseActor(a, out, err)) { *err = "actor: " + *err; return false; }
    return true;
}

V3 Vec(const json &j) {
    if (!j.is_array() || j.size() != 3) return {0, 0, 0};
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

} // namespace

void HitsInit() {
    if (!game::g_exe || !E().pedBulletHit) { g_bulletWhy = "not known for this build"; return; }
    std::string err;
    g_bulletHooked = HookAdd(nullptr, E().pedBulletHit, reinterpret_cast<void *>(&hkPedBullet), reinterpret_cast<void **>(&oPedBullet), &err);
    g_bulletWhy = g_bulletHooked ? "hooked" : err;
    if (!g_bulletHooked) HWARN("hits: the pedestrians' bullet Hit is not hooked (a bullet on a pedestrian is filtered in pedHit, after the element woke): %s", err.c_str());
}

// the bridge's filter: after the mods', a hit on whom an agent's hits.take asks for is taken
bool BridgeTakes(void *person) {
    DWORD now = GetTickCount();
    g_takes.erase(std::remove_if(g_takes.begin(), g_takes.end(), [&](const Take &t) { return static_cast<int>(t.until - now) <= 0; }),
                  g_takes.end());
    for (Take &t : g_takes)
        if (!t.who || t.who == person) { t.taken++; return true; }
    return false;
}

bool HitsFilter(void *person, int ped, MmlHumanHit *hit) {
    if ((g_filters.empty() && g_takes.empty()) || g_inFilter) return false;
    g_seen++;
    std::vector<Filter> filters = g_filters;   // (a filter may set or remove filters)
    g_inFilter++;
    bool taken = false;
    for (const Filter &f : filters) {
        if (!ModAlive(f.mod)) continue;
        FilterCall c{&f, person, ped, hit, MML_HIT_GAME};
        if (!ModCall(f.mod, CallFilter, &c, "its hit filter")) { g_faults++; continue; }
        if (c.r == MML_HIT_TAKEN) { taken = true; break; }
    }
    g_inFilter--;
    if (!taken && !g_takes.empty()) taken = BridgeTakes(person);
    if (taken) { g_taken++; hit->taken = 1; }
    return taken;
}

bool HitsInPedBullet(void *element) { return element && element == g_inBullet; }

int HitFilterSet(MmlMod *mod, MmlHitFilterFn fn, void *user) {
    g_filters.erase(std::remove_if(g_filters.begin(), g_filters.end(), [&](const Filter &f) { return f.mod == mod; }), g_filters.end());
    if (fn) g_filters.push_back({mod, fn, user});
    return 1;
}

int HumanHitCall(void *person, const MmlHumanHit *hit, std::string *err) {
    if (!game::g_exe) { *err = "no game"; return 0; }
    if (!person || !hit) { *err = "no person or no hit"; return 0; }
    V3 dir{hit->dir.x, hit->dir.y, hit->dir.z}, pos{hit->pos.x, hit->pos.y, hit->pos.z}, normal{0, 0, 0};
    bool done = false;
    if (HumanKnown(person)) {
        uint32_t type = 0;
        auto t = [&] { type = game::at<uint32_t>(person, game::kActorType); };
        if (!Guarded(t) || (type != 2 && type != 27)) { *err = "not a person (a C_human) the loader knows"; return 0; }
        using HitFn = int(__fastcall *)(void *, void *, int, V3 *, V3 *, V3 *, float, void *, unsigned, void *);
        auto f = [&] {
            auto fn = reinterpret_cast<HitFn>((*reinterpret_cast<void ***>(person))[kVtHit / 4]);
            fn(person, nullptr, hit->type, &dir, &pos, &normal, hit->damage, hit->attacker, static_cast<unsigned>(hit->part), nullptr);
        };
        done = Guarded(f);
    } else if (PedestrianKnown(person)) {
        if (!E().pedHit) { *err = "the pedestrians' Hit is not known for this build"; return 0; }
        using PedHitFn = void(__fastcall *)(void *, void *, int, V3 *, int, void *, float);
        auto f = [&] { reinterpret_cast<PedHitFn>(E().pedHit)(person, nullptr, hit->type, &dir, hit->part, hit->attacker, hit->damage); };
        done = Guarded(f);
    } else {
        *err = "neither a person (humans_list) nor a traffic pedestrian (pedestrians_list)";
        return 0;
    }
    if (!done) { *err = "the game's Hit faulted"; return 0; }
    g_called++;
    return 1;
}

void HitsModGone(MmlMod *mod) { HitFilterSet(mod, nullptr, nullptr); }

bool HitsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "human.hit" && method != "hits.state" && method != "hits.take") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "hits.state") {
            json filters = json::array();
            for (const Filter &f : g_filters) filters.push_back(ModIdOf(f.mod));
            DWORD now = GetTickCount();
            json takes = json::array();
            for (const Take &t : g_takes)
                if (static_cast<int>(t.until - now) > 0)
                    takes.push_back({{"who", t.who ? Hex(t.who) : "everyone"}, {"ms_left", static_cast<int>(t.until - now)}, {"taken", t.taken}});
            *reply = dump({{"filters", filters}, {"bridge_takes", takes}, {"seen", g_seen}, {"taken", g_taken}, {"faults", g_faults}, {"human_hit_calls", g_called},
                           {"pedestrian_bullets", g_bulletWhy}});
            *ok = true;
            return true;
        }
        if (method == "hits.take") {   // an agent's test: whom the bridge's filter takes hits from, and for how long
            if (p.value("clear", false)) {
                g_takes.clear();
                *reply = dump({{"cleared", true}});
                *ok = true;
                return true;
            }
            void *t = nullptr;
            std::string e;
            if (!p.value("all", false)) {
                if (!Person(p, &t, &e)) { *reply = "hits.take: " + e; return true; }
                if (!t) { *reply = "hits.take: no such person (no game, or no player)"; return true; }
            }
            int ms = std::min(std::max(p.value("ms", 10000), 1), 600000);
            g_takes.push_back({t, GetTickCount() + static_cast<DWORD>(ms), 0});
            *reply = dump({{"who", t ? Hex(t) : "everyone"}, {"ms", ms}});
            *ok = true;
            return true;
        }
        void *who = nullptr;
        std::string err;
        if (!Person(p, &who, &err)) { *reply = "human.hit: " + err; return true; }
        MmlHumanHit h{};
        h.type = p.value("type", 0);
        h.damage = p.value("damage", 0.0f);
        h.part = p.value("part", 0);
        V3 d = Vec(p.value("dir", json::array({0, 0, 0}))), at = Vec(p.value("pos", json::array({0, 0, 0})));
        h.dir = {d.x, d.y, d.z};
        h.pos = {at.x, at.y, at.z};
        if (p.contains("attacker")) {
            std::string at2 = p.value("attacker", "");
            if (at2 == "player") h.attacker = HumanPlayer();
            else if (!ParseActor(at2, &h.attacker, &err)) { *reply = "human.hit: attacker: " + err; return true; }
        }
        MmlPedestrian before{}, after{};
        bool isPed = PedestrianKnown(who);
        bool wasAlive = isPed ? (PedestrianRead(who, &before), before.alive != 0) : AliveFlag(who);
        if (!HumanHitCall(who, &h, &err)) { *reply = "human.hit: " + err; return true; }
        bool alive = isPed ? (PedestrianRead(who, &after), after.alive != 0) : AliveFlag(who);
        json r = {{"who", Hex(who)}, {"pedestrian", isPed}, {"was_alive", wasAlive}, {"alive", alive}};
        if (isPed) r["health"] = after.health;
        *reply = dump(r);
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
