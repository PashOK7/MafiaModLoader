// What happens in the game, as events (mml.h MML_EVENT_SHOT and the others; docs/MODDING.md "What happens").
//
// - SHOT: C_game::NewShoot (game_build.h newShoot), where every weapon's shot goes into the game's shot queue: the
//   shooter, where from, the direction, the damage, how many pellets. The game resolves it after (what it strikes
//   comes as HUMAN_HIT for people, BULLET_HIT for the rest).
// - BULLET_HIT: NewShoot finds the static world's impact at once (testLine) and keeps it in the bullet's record in
//   the shot queue (game.h kGameShots); the bullets' tick (C_game::TickShoot) moves each bullet and asks collRayTest
//   (its call returns to bulletRay) for the moving objects on the frame's stretch. What that returns struck, unless
//   it is a person (the game then tests his parts and hits him or lets the bullet go on: HUMAN_HIT, PEDESTRIAN_HIT
//   tell it) or the shooter; with nothing moving on the way, a bullet come to its static impact struck there (the
//   tick's own test). The record is the one now at the stretch's end: its shooter, damage, direction, impact.
// - EXPLOSION: C_game::NewExplosion (newExplosion): grenades, cars, the scripts' explosions: the source, where, the
//   radius and the damage.
// - CAR_EXPLODED: C_car::CarExplosion (carExplosion), the car.
// - CAR_CRASH: C_car::Was_Collision (carCrash), each contact of a car's body or wheels with the world or another car
//   (each car of the two gets its own); a car pressed on a wall touches it every frame, so only contacts that change
//   the car's speed by 1 m/s or more are told (the game's own sparks begin at 5, its debris at 6).
// - HUMAN_DIED, VEHICLE_ENTERED, VEHICLE_LEFT, HUMAN_FELL, ITEM_GAINED, ITEM_LOST: every frame each person the host
//   knows (humans.cpp) is looked at: his alive flag (+0x5D) going out is a death however it came (a hit, a fall, the
//   water, a script's human_death), with the last who hit him in the 3 s before as the killer; the vehicle he uses
//   (+0x98) changing is an entry or a leaving; his state leaving 3 (the air) is a landing, with the fall's fastest
//   speed and its height from the highest point; his inventory's items (by id: their count, their rounds) changing is
//   an item gained or lost (moved between his slots is no change, rounds spent are not told). MONEY_CHANGED: the
//   freeride's money (C_game +0x3640) the same way. Read, not hooked: every way into a car, out of life or into the
//   inventory is seen alike, a frame late at most.
#include "host.h"
#include <intrin.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace host {
namespace {

using NewShootFn = char(__fastcall *)(void *game, void *edx, void *actor, MmlVec3 pos, MmlVec3 dir, float damage, int particle,
                                      void *frame, int shots);
using NewExplosionFn = void(__fastcall *)(void *game, void *edx, void *source, MmlVec3 *pos, float radius, float damage, bool world,
                                          bool large, bool smoke, int sound);
using CarExplosionFn = char(__fastcall *)(void *car, void *edx, uint32_t flags);
using RayFn = void *(__fastcall *)(void *coll, void *edx, const V3 *from, const V3 *delta, V3 *hit, V3 *normal, int flag);
using CarCrashFn = char(__fastcall *)(void *car, const V3 *impulse, const V3 *a3, const V3 *pos, float a5, const V3 *a6, void *struck,
                                      const uint32_t *prim, int a9);
NewShootFn oNewShoot;
NewExplosionFn oNewExplosion;
CarExplosionFn oCarExplosion;
RayFn oRay;
CarCrashFn oCarCrash;
bool g_hooked, g_rayHooked, g_crashHooked;
uintptr_t g_bulletRay;

constexpr float kCrashSpeed = 1.0f;   // m/s: the least change of a car's speed told as CAR_CRASH
constexpr int kKinds = MML_SLOT_COUNT;

struct Inv { int n; int id[kKinds], count[kKinds], rounds[kKinds]; };
struct Seen {
    void *human; bool alive; void *vehicle; void *lastHitBy; DWORD lastHitAt;
    bool air; float top, speed;   // in the air: the fall's highest point, its fastest speed
    bool inv; Inv items;          // inv: the items were read once
};
std::vector<Seen> g_seen;
int g_money = INT_MIN;
// A frame's events, sent once the people are looked at (a mod's handler may touch g_seen: a hit, a person made).
struct Pending { MmlEvent ev; MmlFall fall; MmlItemChange item; };
std::vector<Pending> g_pending;

MmlEvent Ev(MmlEventType type) {
    MmlEvent ev{};
    ev.size = sizeof ev;
    ev.type = type;
    return ev;
}

char __fastcall hkNewShoot(void *game, void *edx, void *actor, MmlVec3 pos, MmlVec3 dir, float damage, int particle, void *frame, int shots) {
    char r = oNewShoot(game, edx, actor, pos, dir, damage, particle, frame, shots);
    MmlShot s{actor, pos, dir, damage, shots};
    MmlEvent ev = Ev(MML_EVENT_SHOT);
    ev.actor = actor;
    ev.shot = &s;
    Dispatch(ev);
    return r;
}
void __fastcall hkNewExplosion(void *game, void *edx, void *source, MmlVec3 *pos, float radius, float damage, bool world, bool large,
                               bool smoke, int sound) {
    oNewExplosion(game, edx, source, pos, radius, damage, world, large, smoke, sound);
    MmlExplosion x{source, pos ? *pos : MmlVec3{}, radius, damage};
    MmlEvent ev = Ev(MML_EVENT_EXPLOSION);
    ev.actor = source;
    ev.explosion = &x;
    Dispatch(ev);
}
char __fastcall hkCarExplosion(void *car, void *edx, uint32_t flags) {
    char r = oCarExplosion(car, edx, flags);
    MmlEvent ev = Ev(MML_EVENT_CAR_EXPLODED);
    ev.actor = car;
    Dispatch(ev);
    return r;
}

// The last static impact told: the tick may test a bullet's stretch again in the same frame (after a person it
// passed), and its record is gone once its impact is made.
V3 g_toldFrom{};
void *g_toldShooter;

// After a bullet's stretch was tested (collRayTest gave `prim`, or null): what it struck and whose bullet, or false
// (nothing; a person, whose hit the game tells; the shooter; a fault).
bool ReadBullet(const uint32_t *prim, const V3 &from, const V3 &delta, const V3 &at, const V3 &n, MmlBulletHit *b) {
    __try {
        const game::ExeBuild &x = game::Exe();
        void *owner = prim && (prim[0] & 0xC0) == 0x40 ? game::at<void *>(const_cast<uint32_t *>(prim), 0x44) : nullptr;
        if (owner) {
            int t = game::at<int>(owner, game::kActorType);
            if (t == 2 || t == 27) return false;                                              // a person's parts: HUMAN_HIT
            if (x.pedSystemVtable && game::at<uintptr_t>(owner, 0) == x.pedSystemVtable) return false;   // PEDESTRIAN_HIT
        }
        // the bullet's record: the one now at the stretch's end
        V3 end = from + delta;
        uint8_t *rec = nullptr;
        float best = 0.01f;
        if (void *g = game::Game()) {
            uint8_t *r0 = game::at<uint8_t *>(g, game::kGameShots), *r1 = game::at<uint8_t *>(g, game::kGameShots + 4);
            if (r0 && r1 > r0 && (r1 - r0) / game::kShotSize < 4096)
                for (uint8_t *r = r0; r + game::kShotSize <= r1; r += game::kShotSize) {
                    V3 d = game::at<V3>(r, game::kShotNow) - end;
                    float d2 = Dot(d, d);
                    if (d2 < best) { best = d2; rec = r; }
                }
        }
        void *shooter = rec ? game::at<void *>(rec, game::kShotShooter) : nullptr;
        float damage = rec ? game::at<float>(rec, game::kShotDamage) : 0.0f;
        V3 way = rec ? game::at<V3>(rec, game::kShotWay) : delta;
        V3 dir = way;
        float l = std::sqrt(Dot(dir, dir));
        if (l > 1e-6f) dir = dir * (1.0f / l);
        if (prim) {
            if (owner && owner == shooter) return false;   // (the game passes over the shooter's own objects)
            *b = MmlBulletHit{shooter, {at.x, at.y, at.z}, {n.x, n.y, n.z}, {dir.x, dir.y, dir.z}, damage, owner, prim[0], prim};
            return true;
        }
        // nothing moving on the way: the static impact, when the bullet has come to it (the tick's own test)
        if (!rec || !game::at<uint8_t>(rec, game::kShotHits)) return false;
        V3 start = game::at<V3>(rec, game::kShotFrom), gone = game::at<V3>(rec, game::kShotNow) - start;
        if (Dot(gone, gone) + 1.0f < Dot(way, way)) return false;
        if (shooter == g_toldShooter && start.x == g_toldFrom.x && start.y == g_toldFrom.y && start.z == g_toldFrom.z) return false;
        g_toldFrom = start;
        g_toldShooter = shooter;
        const uint32_t *sp = game::at<const uint32_t *>(rec, game::kShotPrim);
        uint32_t flags = sp ? sp[0] : static_cast<uint32_t>(game::at<uint8_t>(rec, game::kShotSurface)) << 24;
        V3 hp = game::at<V3>(rec, game::kShotHitPos), hn = game::at<V3>(rec, game::kShotHitNormal);
        *b = MmlBulletHit{shooter, {hp.x, hp.y, hp.z}, {hn.x, hn.y, hn.z}, {dir.x, dir.y, dir.z}, damage, nullptr, flags, sp};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void *__fastcall hkRay(void *coll, void *edx, const V3 *from, const V3 *delta, V3 *hit, V3 *normal, int flag) {
    void *r = oRay(coll, edx, from, delta, hit, normal, flag);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_bulletRay) {
        MmlBulletHit b{};
        if (ReadBullet(static_cast<const uint32_t *>(r), *from, *delta, *hit, *normal, &b)) {
            MmlEvent ev = Ev(MML_EVENT_BULLET_HIT);
            ev.actor = b.shooter;
            ev.bullet = &b;
            Dispatch(ev);
        }
    }
    return r;
}

bool ReadCrash(void *car, const V3 *impulse, const V3 *pos, const uint32_t *prim, MmlCarCrash *c) {
    __try {
        const game::ExeBuild &x = game::Exe();
        float mass = game::at<float>(car, x.vehicle + x.vehMass);
        float j = std::sqrt(Dot(*impulse, *impulse));
        if (!(mass > 0) || j / mass < kCrashSpeed) return false;
        *c = MmlCarCrash{{pos->x, pos->y, pos->z}, {impulse->x, impulse->y, impulse->z}, j / mass, prim ? prim[0] : 0u};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

char __fastcall hkCarCrash(void *car, const V3 *impulse, const V3 *a3, const V3 *pos, float a5, const V3 *a6, void *struck,
                           const uint32_t *prim, int a9) {
    char r = oCarCrash(car, impulse, a3, pos, a5, a6, struck, prim, a9);
    MmlCarCrash c{};
    if (impulse && pos && ReadCrash(car, impulse, pos, prim, &c)) {
        MmlEvent ev = Ev(MML_EVENT_CAR_CRASH);
        ev.actor = car;
        ev.other = struck;
        ev.crash = &c;
        Dispatch(ev);
    }
    return r;
}

struct State { bool ok; bool alive; void *vehicle; int state; float fall; float y; };
State ReadState(void *h) {
    State s{};
    __try {
        s.alive = game::at<uint8_t>(h, game::kActorAlive) != 0;
        s.vehicle = game::at<void *>(h, game::kHumanVehicle);
        int st = game::Exe().humanState;
        s.state = st ? game::at<int>(h, st) : 0;
        s.fall = game::at<float>(h, game::kHumanFallSpeed);
        s.y = game::ActorWorldPos(h).y;
        s.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.ok = false;
    }
    return s;
}

// A person's items by id: how many and their rounds.
Inv ReadItems(void *h) {
    Inv v{};
    MmlItem items[MML_SLOT_COUNT];
    int n = std::min(StateItems(h, items, MML_SLOT_COUNT), static_cast<int>(MML_SLOT_COUNT));
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < v.n && v.id[k] != items[i].id) k++;
        if (k == v.n) { v.id[k] = items[i].id; v.count[k] = 0; v.rounds[k] = 0; v.n++; }
        v.count[k]++;
        v.rounds[k] += items[i].loaded + items[i].reserve;
    }
    return v;
}
int Find(const Inv &v, int id) {
    for (int k = 0; k < v.n; k++) if (v.id[k] == id) return k;
    return -1;
}
void Queue(MmlEventType type, void *actor, void *other) {
    Pending p{};
    p.ev = Ev(type);
    p.ev.actor = actor;
    p.ev.other = other;
    g_pending.push_back(p);
}
void TellItem(void *h, int id, int count, int rounds) {
    Queue(count < 0 ? MML_EVENT_ITEM_LOST : MML_EVENT_ITEM_GAINED, h, nullptr);
    g_pending.back().item = MmlItemChange{id, count, rounds};
}
// What changed from `was` to `now`: items gained or lost, rounds gained for an item he had.
void TellItems(void *h, const Inv &was, const Inv &now) {
    for (int k = 0; k < now.n; k++) {
        int j = Find(was, now.id[k]);
        int dc = now.count[k] - (j < 0 ? 0 : was.count[j]), dr = now.rounds[k] - (j < 0 ? 0 : was.rounds[j]);
        if (dc != 0) TellItem(h, now.id[k], dc, dr);
        else if (dr > 0) TellItem(h, now.id[k], 0, dr);
    }
    for (int j = 0; j < was.n; j++)
        if (Find(now, was.id[j]) < 0) TellItem(h, was.id[j], -was.count[j], -was.rounds[j]);
}
bool SameItems(const Inv &a, const Inv &b) {
    if (a.n != b.n) return false;
    for (int k = 0; k < a.n; k++) {
        int j = Find(b, a.id[k]);
        if (j < 0 || b.count[j] != a.count[k] || b.rounds[j] != a.rounds[k]) return false;
    }
    return true;
}

} // namespace

void HappenInit() {
    if (g_hooked || !game::g_exe) return;
    const game::ExeBuild &x = game::Exe();
    if (!x.newShoot || !x.newExplosion || !x.carExplosion) return;
    std::string err;
    bool ok = HookAdd(nullptr, x.newShoot, reinterpret_cast<void *>(&hkNewShoot), reinterpret_cast<void **>(&oNewShoot), &err) &&
              HookAdd(nullptr, x.newExplosion, reinterpret_cast<void *>(&hkNewExplosion), reinterpret_cast<void **>(&oNewExplosion), &err) &&
              HookAdd(nullptr, x.carExplosion, reinterpret_cast<void *>(&hkCarExplosion), reinterpret_cast<void **>(&oCarExplosion), &err);
    if (!ok) { HWARN("happen: the shots', explosions' hooks: %s (those events are not sent)", err.c_str()); return; }
    g_hooked = true;
    if (x.collRayTest && x.bulletRay) {
        g_bulletRay = x.bulletRay;
        g_rayHooked = HookAdd(nullptr, x.collRayTest, reinterpret_cast<void *>(&hkRay), reinterpret_cast<void **>(&oRay), &err);
        if (!g_rayHooked) HWARN("happen: the bullets' ray: %s (no BULLET_HIT)", err.c_str());
    }
    if (x.carCrash) {
        g_crashHooked = HookAdd(nullptr, x.carCrash, reinterpret_cast<void *>(&hkCarCrash), reinterpret_cast<void **>(&oCarCrash), &err);
        if (!g_crashHooked) HWARN("happen: the cars' crashes: %s (no CAR_CRASH)", err.c_str());
    }
}

// (game_hooks.cpp HUMAN_HIT) who hit whom: the killer of a death that follows.
void HappenHit(void *human, void *attacker) {
    if (!attacker) return;
    for (Seen &s : g_seen) if (s.human == human) { s.lastHitBy = attacker; s.lastHitAt = GetTickCount(); return; }
    Seen s{};
    s.human = human; s.alive = true; s.lastHitBy = attacker; s.lastHitAt = GetTickCount();
    g_seen.push_back(s);
}

// (game_hooks.cpp) every frame of a mission: deaths, vehicles, landings, items, the money.
void HappenFrame() {
    void *list[1024];
    int n = std::min(HumansList(list, 1024), 1024);
    // the people gone since: their records go
    g_seen.erase(std::remove_if(g_seen.begin(), g_seen.end(), [&](const Seen &s) { return std::find(list, list + n, s.human) == list + n; }),
                 g_seen.end());
    for (int i = 0; i < n; i++) {
        void *h = list[i];
        State st = ReadState(h);
        if (!st.ok) continue;
        auto it = std::find_if(g_seen.begin(), g_seen.end(), [&](const Seen &s) { return s.human == h; });
        if (it == g_seen.end()) {
            Seen s{};
            s.human = h; s.alive = st.alive; s.vehicle = st.vehicle;
            s.inv = true; s.items = ReadItems(h);
            g_seen.push_back(s);
            continue;
        }
        Seen &s = *it;
        if (s.alive && !st.alive) {
            Queue(MML_EVENT_HUMAN_DIED, h, s.lastHitBy && GetTickCount() - s.lastHitAt < 3000 ? s.lastHitBy : nullptr);
            s.alive = false;
        } else if (!s.alive && st.alive) {
            s.alive = true;   // (brought back: a mission's restart)
        }
        if (st.vehicle != s.vehicle) {
            void *was = s.vehicle;
            s.vehicle = st.vehicle;
            s.air = false;
            if (was) Queue(MML_EVENT_VEHICLE_LEFT, h, was);
            if (st.vehicle) Queue(MML_EVENT_VEHICLE_ENTERED, h, st.vehicle);
        }
        // the air: a landing when the state leaves it (on foot only: a seated person's state is the car's business)
        if (!st.vehicle && st.state == game::kHumanStateAir) {
            if (!s.air) { s.air = true; s.top = st.y; s.speed = 0; }
            s.top = std::max(s.top, st.y);
            s.speed = std::max(s.speed, st.fall);
        } else if (s.air) {
            s.air = false;
            Queue(MML_EVENT_HUMAN_FELL, h, nullptr);
            g_pending.back().fall = MmlFall{std::max(s.speed, st.fall), std::max(0.0f, s.top - st.y)};
        }
        Inv now = ReadItems(h);
        if (!s.inv) { s.inv = true; s.items = now; }
        else if (!SameItems(s.items, now)) {
            Inv was = s.items;
            s.items = now;
            TellItems(h, was, now);
        }
    }
    int m = StateMoney();
    if (m != INT_MIN && g_money != INT_MIN && m != g_money) {
        Queue(MML_EVENT_MONEY_CHANGED, nullptr, nullptr);
        g_pending.back().ev.value = m;
        g_pending.back().ev.change = m - g_money;
    }
    g_money = m;
    // sent now, each with its own payload (the list is taken first: a handler's call may come back here)
    std::vector<Pending> sent;
    sent.swap(g_pending);
    for (Pending &p : sent) {
        if (p.ev.type == MML_EVENT_HUMAN_FELL) p.ev.fall = &p.fall;
        if (p.ev.type == MML_EVENT_ITEM_GAINED || p.ev.type == MML_EVENT_ITEM_LOST) p.ev.item = &p.item;
        Dispatch(p.ev);
    }
}

void HappenMissionClosed() { g_seen.clear(); g_pending.clear(); g_money = INT_MIN; }

} // namespace host
