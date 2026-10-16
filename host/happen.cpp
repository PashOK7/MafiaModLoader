// What happens in the game, as events (mml.h MML_EVENT_SHOT and the others; docs/MODDING.md "What happens").
//
// - SHOT: C_game::NewShoot (game_build.h newShoot), where every weapon's shot goes into the game's shot queue: the
//   shooter, where from, the direction, the damage, how many pellets. The game resolves it after (what it strikes
//   comes as HUMAN_HIT for people).
// - EXPLOSION: C_game::NewExplosion (newExplosion): grenades, cars, the scripts' explosions: the source, where, the
//   radius and the damage.
// - CAR_EXPLODED: C_car::CarExplosion (carExplosion), the car.
// - HUMAN_DIED, VEHICLE_ENTERED, VEHICLE_LEFT: every frame each person the host knows (humans.cpp) is looked at: his
//   alive flag (+0x5D) going out is a death however it came (a hit, a fall, the water, a script's human_death), with
//   the last who hit him in the 3 s before as the killer; the vehicle he uses (+0x98) changing is an entry or a
//   leaving. Read, not hooked: every way into a car or out of life is seen alike, a frame late at most.
#include "host.h"
#include <algorithm>
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
NewShootFn oNewShoot;
NewExplosionFn oNewExplosion;
CarExplosionFn oCarExplosion;
bool g_hooked;

struct Seen { void *human; bool alive; void *vehicle; void *lastHitBy; DWORD lastHitAt; };
std::vector<Seen> g_seen;

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

struct State { bool ok; bool alive; void *vehicle; };
State ReadState(void *h) {
    State s{};
    __try {
        s.alive = game::at<uint8_t>(h, game::kActorAlive) != 0;
        s.vehicle = game::at<void *>(h, game::kHumanVehicle);
        s.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.ok = false;
    }
    return s;
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
}

// (game_hooks.cpp HUMAN_HIT) who hit whom: the killer of a death that follows.
void HappenHit(void *human, void *attacker) {
    if (!attacker) return;
    for (Seen &s : g_seen) if (s.human == human) { s.lastHitBy = attacker; s.lastHitAt = GetTickCount(); return; }
    g_seen.push_back({human, true, nullptr, attacker, GetTickCount()});
}

// (game_hooks.cpp) every frame of a mission: deaths and vehicles.
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
        if (it == g_seen.end()) { g_seen.push_back({h, st.alive, st.vehicle, nullptr, 0}); continue; }
        Seen &s = *it;
        if (s.alive && !st.alive) {
            MmlEvent ev = Ev(MML_EVENT_HUMAN_DIED);
            ev.actor = h;
            ev.other = s.lastHitBy && GetTickCount() - s.lastHitAt < 3000 ? s.lastHitBy : nullptr;
            s.alive = false;
            Dispatch(ev);
        } else if (!s.alive && st.alive) {
            s.alive = true;   // (brought back: a mission's restart)
        }
        if (st.vehicle != s.vehicle) {
            void *was = s.vehicle;
            s.vehicle = st.vehicle;
            if (was) { MmlEvent ev = Ev(MML_EVENT_VEHICLE_LEFT); ev.actor = h; ev.other = was; Dispatch(ev); }
            if (st.vehicle) { MmlEvent ev = Ev(MML_EVENT_VEHICLE_ENTERED); ev.actor = h; ev.other = st.vehicle; Dispatch(ev); }
        }
    }
}

void HappenMissionClosed() { g_seen.clear(); }

} // namespace host
