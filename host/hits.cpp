// What strikes people, before the game's reaction (mml.h hit_filter, human_hit; docs/MODDING.md "Hits before the
// game").
//
// Every hit a person takes passes the mods' filters before the game reacts to it: C_human::Hit (game_hooks.cpp
// hkHumanHit: bullets, explosions, cars, falls, the scripts), the traffic pedestrians' Hit (anims.cpp hkPedHit: pedHit,
// thiscall(element, type, dir, part, attacker, float damage): the traffic generator's fire and explosions, pedCarHit,
// the bullets), and before that, for a bullet, the pedestrian system's own Hit (game_build.h pedBulletHit, the vtable
// +0x7C of every pedestrian's collision object's owner, thiscall(system, type, dir, from, pos, float damage, attacker,
// ?, model frame) ret 0x20): it finds the element by the model struck and wakes it (pedWake: a dead one would cry out at
// its next tick) before it calls pedHit, so the bullet is filtered there, once, and pedHit's own filter is passed by
// while it runs. For a dead element 1.2's goes no further (1.0's calls pedHit, which returns at once): the host tells
// the bullet as PEDESTRIAN_HIT itself then, so a bullet on a corpse is told in both builds. The filters run in the order they were set; each sees the hit (MmlHumanHit) and may change its
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
// Bullets on a mod's shapes (bullet_shape). The bullets' tick (bulletsTick) moves each bullet a stretch a tick and asks
// the collision world what the stretch meets (collRayTest, thiscall(g_collision, from, delta, out hit, out normal,
// int) -> primitive, its +0x44 the owner; game_build.h bulletRay its call's return), with the objects it has gone
// through already in the world's exclusion list (g_collision +0x2B8 the array, +0x2BC the count, both builds:
// DynSetNextCollOwnerMult). A dead person is not in that world any more, so a body a mod moves (a ragdoll) is missed.
// While any mod has given a person's body as capsules, collRayTest is hooked (outermost: the return address is the
// game's caller's) and, for the bullets' call alone, the stretch is also tested against them: the nearest capsule
// nearer than what the game found is given back as a hit on that person, his own collision object (a C_human's first of
// humanColl, its owner him; a pedestrian's the element's +0xC4, its owner the pedestrian system) with the point and the
// normal on the capsule. The game then goes on as for a living person: a C_human's part by its own part test (asked
// first here, so the game never gets a hit it would not take), the blood, the Hit (and the mods' filters: the hit's
// `shape` says which capsule). An object already in the exclusion list is not given (the tick tests the same stretch
// again after each hit, and a body given again for ever hung the game in mafia-ragdoll), and the same stretch giving
// the same object more than 4 times is left to the game. Mafia-ragdoll's hits.cpp, both builds' bulletsTick read.
//
// Bridge methods (game thread): human.hit {actor | pedestrian, type, dir, pos, damage, attacker, part}, hits.take
// {actor | pedestrian | all, ms, clear}, hits.shape {actor | pedestrian, capsules, clear}, hits.state.
#include "host.h"
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
bool g_bulletToPedHit;       // ... and the game's code went on to pedHit (its PEDESTRIAN_HIT told there)
unsigned g_seen, g_taken, g_faults, g_called;
bool g_bulletHooked;
std::string g_bulletWhy = "not tried";

using PedBulletFn = int(__fastcall *)(void *sys, void *edx, int type, V3 *dir, V3 *from, V3 *pos, float damage, void *attacker,
                                      int a7, void *frame);
PedBulletFn oPedBullet;

// ---- bullets on the mods' shapes
constexpr int kExclArray = 0x2B8, kExclCount = 0x2BC, kObjFlags = 0x08, kObjOwner = 0x44, kPedColl = 0xC4;
constexpr uint32_t kObjPerson = 0x100;
struct Shape { MmlMod *mod; void *person; bool pedestrian; std::vector<MmlCapsule> parts; V3 centre; float radius; };
std::vector<Shape> g_shapes;
using RayFn = void *(__fastcall *)(void *coll, void *edx, const V3 *from, const V3 *delta, V3 *hit, V3 *normal, int flag);
RayFn oRay;
bool g_rayHooked;
std::string g_rayWhy = "not needed yet (no shapes)";
struct Last { void *person; int shape; };
Last g_last;                 // the capsule the last bullet stretch was given (the Hit that follows tells it)
struct Repeat { V3 from, delta; void *obj; int n; };
Repeat g_repeat;
unsigned g_stretches, g_shapeHits, g_partMisses, g_excluded, g_repeats, g_rayFaults;

V3 At(const float *p) { return {p[0], p[1], p[2]}; }

// the distance along the unit `d` from `o` to where the ray enters the capsule a-e of radius r, or -1 (exact: the
// cylinder clipped by the axis, then the end spheres)
float RayCapsule(const V3 &o, const V3 &d, const V3 &a, const V3 &e, float r) {
    V3 ba = e - a, oa = o - a;
    float baba = Dot(ba, ba), bard = Dot(ba, d), baoa = Dot(ba, oa), rdoa = Dot(d, oa), oaoa = Dot(oa, oa);
    float A = baba - bard * bard, B = baba * rdoa - baoa * bard, C = baba * oaoa - baoa * baoa - r * r * baba;
    if (A > 1e-9f) {
        float h = B * B - A * C;
        if (h < 0) return -1;
        float t = (-B - std::sqrt(h)) / A;
        float y = baoa + t * bard;
        if (t >= 0 && y > 0 && y < baba) return t;
    }
    float best = -1;
    for (const V3 &c : {a, e}) {
        V3 oc = o - c;
        float b = Dot(d, oc), cc = Dot(oc, oc) - r * r, h = b * b - cc;
        if (h < 0) continue;
        float t = -b - std::sqrt(h);
        if (t >= 0 && (best < 0 || t < best)) best = t;
    }
    return best;
}
V3 CapsuleNormal(const V3 &p, const V3 &a, const V3 &e) {
    V3 ba = e - a;
    float l2 = Dot(ba, ba);
    float h = l2 > 1e-9f ? std::min(1.0f, std::max(0.0f, Dot(p - a, ba) / l2)) : 0.0f;
    return Norm(p - (a + ba * h), {0, 1, 0});
}

// the game's own collision object of a person, as its ray test returns it for him alive; null if it is not there
void *ObjectOf(const Shape &s) {
    uint8_t *o = nullptr;
    if (s.pedestrian) {
        o = static_cast<uint8_t *>(s.person) + kPedColl;
    } else {
        int n = game::at<int>(s.person, E().humanCollCount);
        void **arr = game::at<void **>(s.person, E().humanColl);
        if (n <= 0 || !arr || !arr[0]) return nullptr;
        o = static_cast<uint8_t *>(arr[0]);
        if (game::at<void *>(o, kObjOwner) != s.person) return nullptr;
    }
    if (!(game::at<uint32_t>(o, kObjFlags) & kObjPerson) || !game::at<void *>(o, kObjOwner)) return nullptr;
    return o;
}
bool Excluded(void *coll, const void *obj) {
    int n = game::at<int>(coll, kExclCount);
    void *const *arr = game::at<void *const *>(coll, kExclArray);
    for (int i = 0; arr && i < n && i < 4096; i++)
        if (arr[i] == obj) return true;
    return false;
}

void *Bullet(void *coll, void *r, const V3 *from, const V3 *delta, V3 *hit, V3 *normal) {
    g_stretches++;
    float len = Len(*delta);
    if (len < 1e-5f) return r;
    V3 dir = *delta * (1.0f / len);
    float bestT = r ? std::min(len, Dot(*hit - *from, dir)) : len;
    const Shape *best = nullptr;
    void *bestObj = nullptr;
    int bestPart = -1;
    V3 bestN{0, 1, 0};
    for (const Shape &s : g_shapes) {
        float tc = std::min(len, std::max(0.0f, Dot(s.centre - *from, dir)));
        if (Len(*from + dir * tc - s.centre) > s.radius) continue;
        void *obj = ObjectOf(s);
        if (!obj) continue;
        if (Excluded(coll, obj)) { g_excluded++; continue; }   // this bullet has gone through him already
        for (size_t k = 0; k < s.parts.size(); k++) {
            const MmlCapsule &c = s.parts[k];
            float t = RayCapsule(*from, dir, At(c.a), At(c.b), c.radius);
            if (t < 0 || t >= bestT) continue;
            bestT = t;
            bestN = CapsuleNormal(*from + dir * t, At(c.a), At(c.b));
            best = &s;
            bestObj = obj;
            bestPart = static_cast<int>(k);
        }
    }
    if (!best) return r;
    // The point given is the ray's nearest to the capsule's axis (inside the body, not its skin): the bullets' tick
    // asks the person's part test again with the length to the point it is given (bulletsTick's humanBoneRayTest
    // call: |hit - from|^2), and a capsule fatter than the part's own volume put the skin's point before the part,
    // so the game dropped the hit (seen 2026-10-21: a shape hit and no Hit). Never past what the game found.
    {
        const MmlCapsule &c = best->parts[bestPart];
        V3 a = At(c.a), u = At(c.b) - a, w = *from - a;
        float uu = Dot(u, u), du = Dot(dir, u), dw = Dot(dir, w), uw = Dot(u, w), den = uu - du * du;
        float t = den > 1e-9f ? (du * uw - uu * dw) / den : bestT;
        float until = std::min(r ? Dot(*hit - *from, dir) - 0.01f : len, bestT + 2 * c.radius);
        bestT = std::max(bestT, std::min(t, until));
    }
    bool same = g_repeat.obj == bestObj && !memcmp(&g_repeat.from, from, sizeof *from) && !memcmp(&g_repeat.delta, delta, sizeof *delta);
    g_repeat = {*from, *delta, bestObj, same ? g_repeat.n + 1 : 1};
    if (g_repeat.n > 4) { g_repeats++; return r; }
    if (!best->pedestrian && E().humanBoneRayTest) {
        // the game's own part test, as the bullets' tick asks it after a hit (to the point given): no part, no hit
        using BoneFn = int(__fastcall *)(void *, void *, const V3 *, const V3 *, float);
        if (!reinterpret_cast<BoneFn>(E().humanBoneRayTest)(static_cast<uint8_t *>(best->person) + E().humanParts, nullptr, from,
                                                              delta, bestT * bestT)) {
            g_partMisses++;
            return r;
        }
    }
    *hit = *from + dir * bestT;
    *normal = bestN;
    g_last = {best->person, bestPart + 1};
    g_shapeHits++;
    return bestObj;
}

void *SafeBullet(void *coll, void *r, const V3 *from, const V3 *delta, V3 *hit, V3 *normal) {
    V3 h0 = *hit, n0 = *normal;
    __try {
        return Bullet(coll, r, from, delta, hit, normal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *hit = h0;
        *normal = n0;
        g_rayFaults++;
        return r;
    }
}

void *__fastcall hkRay(void *coll, void *edx, const V3 *from, const V3 *delta, V3 *hit, V3 *normal, int flag) {
    void *r = oRay(coll, edx, from, delta, hit, normal, flag);
    if (g_shapes.empty() || reinterpret_cast<uintptr_t>(_ReturnAddress()) != E().bulletRay || !from || !delta || !hit || !normal)
        return r;
    g_last = {};
    return SafeBullet(coll, r, from, delta, hit, normal);
}

bool EnsureRay(std::string *err) {
    if (g_rayHooked) return true;
    if (!game::g_exe || !E().collRayTest || !E().bulletRay || !E().humanColl) { *err = g_rayWhy = "bullets not known for this build"; return false; }
    g_rayHooked = HookAddOuter(E().collRayTest, reinterpret_cast<void *>(&hkRay), reinterpret_cast<void **>(&oRay), err);
    g_rayWhy = g_rayHooked ? "hooked" : *err;
    return g_rayHooked;
}

// a person's capsules set (count 0: taken away); one set a person, the last mod's
bool ShapeSet(MmlMod *mod, void *person, const MmlCapsule *parts, int count, std::string *err) {
    bool removing = count <= 0 || !parts;   // (a mod takes away its own; a new set replaces anyone's)
    g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(),
                                  [&](const Shape &s) { return s.person == person && (!removing || s.mod == mod); }),
                   g_shapes.end());
    if (removing) return true;
    bool ped = PedestrianKnown(person);
    if (!ped && !HumanKnown(person)) { *err = "neither a person (humans_list) nor a traffic pedestrian (pedestrians_list)"; return false; }
    if (count > 64) { *err = "64 capsules a person at most"; return false; }
    if (!EnsureRay(err)) return false;
    Shape s{mod, person, ped, std::vector<MmlCapsule>(parts, parts + count), {0, 0, 0}, 0};
    V3 lo = At(parts[0].a), hi = lo;
    for (const MmlCapsule &c : s.parts)
        for (const float *p : {c.a, c.b}) {
            lo = {std::min(lo.x, p[0]), std::min(lo.y, p[1]), std::min(lo.z, p[2])};
            hi = {std::max(hi.x, p[0]), std::max(hi.y, p[1]), std::max(hi.z, p[2])};
        }
    s.centre = (lo + hi) * 0.5f;
    float rmax = 0;
    for (const MmlCapsule &c : s.parts) rmax = std::max(rmax, c.radius);
    s.radius = Len(hi - s.centre) + rmax;
    g_shapes.push_back(std::move(s));
    return true;
}

struct FilterCall { const Filter *f; void *person; int ped; MmlHumanHit *hit; int r; };
void CallFilter(void *p) {
    auto *c = static_cast<FilterCall *>(p);
    c->r = c->f->fn(c->person, c->ped, c->hit, c->f->user);
}

int __fastcall hkPedBullet(void *sys, void *edx, int type, V3 *dir, V3 *from, V3 *pos, float damage, void *attacker, int a7,
                           void *frame) {
    void *element = g_inFilter ? nullptr : PedestrianOfFrame(frame);
    if (!element) return oPedBullet(sys, edx, type, dir, from, pos, damage, attacker, a7, frame);
    MmlHumanHit h{};
    h.type = type;
    if (dir) h.dir = {dir->x, dir->y, dir->z};
    if (pos) h.pos = {pos->x, pos->y, pos->z};
    h.damage = damage;
    h.attacker = attacker;
    h.was_alive = h.alive = PedestrianHealth(element) > 0;
    h.shape = HitsShapeOf(element);
    auto tell = [&] {
        MmlEvent ev{};
        ev.size = sizeof ev;
        ev.type = MML_EVENT_PEDESTRIAN_HIT;
        ev.actor = element;
        ev.other = frame;
        ev.hit = &h;
        Dispatch(ev);
    };
    if (HitsFilter(element, 1, &h)) {
        tell();
        return 1;
    }
    V3 d{h.dir.x, h.dir.y, h.dir.z}, at{h.pos.x, h.pos.y, h.pos.z};
    g_inBullet = element;
    g_bulletToPedHit = false;
    int r = oPedBullet(sys, edx, h.type, dir ? &d : nullptr, from, pos ? &at : nullptr, h.damage, h.attacker, a7, frame);
    g_inBullet = nullptr;
    // 1.2's system Hit goes no further for a dead element (1.0's calls pedHit, which returns at once): the bullet is
    // told all the same, so both builds tell a bullet on a corpse
    if (!g_bulletToPedHit) {
        h.alive = PedestrianHealth(element) > 0;
        tell();
    }
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

bool HitsInPedBullet(void *element) {
    if (!element || element != g_inBullet) return false;
    g_bulletToPedHit = true;
    return true;
}

int HitsShapeOf(void *person) { return person && g_last.person == person ? g_last.shape : 0; }

int BulletShapeSet(MmlMod *mod, void *person, const MmlCapsule *parts, int count, std::string *err) {
    if (!game::g_exe) { *err = "no game"; return 0; }
    return ShapeSet(mod, person, parts, count, err) ? 1 : 0;
}

void HitsActorGone(void *actor) {
    g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(), [&](const Shape &s) { return s.person == actor; }), g_shapes.end());
    g_takes.erase(std::remove_if(g_takes.begin(), g_takes.end(), [&](const Take &t) { return t.who == actor; }), g_takes.end());
    if (g_last.person == actor) g_last = {};
}
void HitsMissionClose() {
    g_shapes.clear();
    g_takes.clear();
    g_last = {};
    g_repeat = {};
}

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

void HitsModGone(MmlMod *mod) {
    HitFilterSet(mod, nullptr, nullptr);
    g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(), [&](const Shape &s) { return s.mod == mod; }), g_shapes.end());
}

bool HitsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "human.hit" && method != "hits.state" && method != "hits.take" && method != "hits.shape") return false;
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
            json shapes = json::array();
            for (const Shape &s : g_shapes)
                shapes.push_back({{"who", Hex(s.person)}, {"pedestrian", s.pedestrian}, {"mod", ModIdOf(s.mod)}, {"capsules", s.parts.size()},
                                  {"object", ObjectOf(s) != nullptr}});
            json bullets = {{"ray", g_rayWhy}, {"stretches", g_stretches}, {"shape_hits", g_shapeHits}, {"part_misses", g_partMisses},
                            {"excluded", g_excluded}, {"repeats", g_repeats}, {"faults", g_rayFaults}};
            *reply = dump({{"filters", filters}, {"bridge_takes", takes}, {"shapes", shapes}, {"bullets", bullets}, {"seen", g_seen}, {"taken", g_taken}, {"faults", g_faults}, {"human_hit_calls", g_called},
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
        if (method == "hits.shape") {   // an agent's test: a person's body as capsules for the bullets, as a mod gives one
            if (!Person(p, &who, &err)) { *reply = "hits.shape: " + err; return true; }
            if (!who) { *reply = "hits.shape: no such person (no game, or no player)"; return true; }
            std::vector<MmlCapsule> caps;
            if (!p.value("clear", false))
                for (const json &c : p.value("capsules", json::array())) {
                    if (!c.is_array() || c.size() != 7) { *reply = "hits.shape: each capsule [ax, ay, az, bx, by, bz, radius]"; return true; }
                    MmlCapsule k{};
                    for (int i = 0; i < 3; i++) { k.a[i] = c[i].get<float>(); k.b[i] = c[3 + i].get<float>(); }
                    k.radius = c[6].get<float>();
                    caps.push_back(k);
                }
            if (!ShapeSet(nullptr, who, caps.data(), static_cast<int>(caps.size()), &err)) { *reply = "hits.shape: " + err; return true; }
            *reply = dump({{"who", Hex(who)}, {"capsules", caps.size()}, {"ray", g_rayWhy}});
            *ok = true;
            return true;
        }
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
