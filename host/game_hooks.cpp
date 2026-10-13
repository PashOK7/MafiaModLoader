// The game's hooks, turned into events (see host.h, mml.h MmlEventType).
//
// Addresses come from sdk/include/mml/game/game_build.h (one table per build; game::Detect picks the running one by
// its code). On a build it does not know, the loader still runs (bridge, mods, memory), with no game events.
#include "host.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace host {
namespace {
const char *g_build = "unknown";
LARGE_INTEGER g_freq, g_last;
bool g_modsStarted;

using FrameLimitFn = int(__stdcall *)(void *, int, int);
using MissionTickFn = int(__fastcall *)(void *, void *, int);
using MissionCloseFn = int(__fastcall *)(void *, void *);
using ActorDtorFn = void(__fastcall *)(void *, void *);
using HumanCtorFn = void *(__fastcall *)(void *, void *);
using HumanHitFn = int(__fastcall *)(void *, void *, int, MmlVec3 *, MmlVec3 *, MmlVec3 *, float, void *, unsigned, void *);
FrameLimitFn oFrameLimit;
MissionTickFn oMissionTick;
MissionCloseFn oMissionClose;
ActorDtorFn oActorDtor;
HumanCtorFn oHumanCtor;
HumanHitFn oHumanHit;

MmlEvent Event(MmlEventType type) {
    MmlEvent ev{};
    ev.size = sizeof ev;
    ev.type = type;
    return ev;
}

// LS3DF's frame limiter: the game's main loop calls it once a frame in every state (intro, menus, loading, missions).
int __stdcall hkFrameLimit(void *driver, int a, int b) {
    int r = oFrameLimit(driver, a, b);
    Frame();
    return r;
}
int __fastcall hkMissionTick(void *self, void *edx, int ms) {
    int r = oMissionTick(self, edx, ms);
    MmlEvent ev = Event(MML_EVENT_MISSION_TICK);
    ev.dt = ms > 0 && ms < 200 ? ms / 1000.0f : 1.0f / 60.0f;
    Dispatch(ev);
    return r;
}
int __fastcall hkMissionClose(void *self, void *edx) {
    Dispatch(Event(MML_EVENT_MISSION_CLOSE));
    return oMissionClose(self, edx);
}
void __fastcall hkActorDtor(void *self, void *edx) {
    MmlEvent ev = Event(MML_EVENT_ACTOR_DESTROYED);
    ev.actor = self;
    Dispatch(ev);
    oActorDtor(self, edx);
}
void *__fastcall hkHumanCtor(void *self, void *edx) {
    void *r = oHumanCtor(self, edx);
    MmlEvent ev = Event(MML_EVENT_HUMAN_CREATED);
    ev.actor = self;
    Dispatch(ev);
    return r;
}
int __fastcall hkHumanHit(void *self, void *edx, int type, MmlVec3 *dir, MmlVec3 *pos, MmlVec3 *normal, float dmg, void *attacker,
                          unsigned part, void *frame) {
    uint8_t before = 0, after = 0;
    MemRead(reinterpret_cast<uintptr_t>(self) + game::kActorAlive, &before, 1);
    int r = oHumanHit(self, edx, type, dir, pos, normal, dmg, attacker, part, frame);
    MemRead(reinterpret_cast<uintptr_t>(self) + game::kActorAlive, &after, 1);
    MmlHumanHit hit{};
    hit.type = type;
    if (dir) hit.dir = *dir;
    if (pos) hit.pos = *pos;
    hit.damage = dmg;
    hit.attacker = attacker;
    hit.was_alive = before != 0;
    hit.alive = after != 0;
    MmlEvent ev = Event(MML_EVENT_HUMAN_HIT);
    ev.actor = self;
    ev.hit = &hit;
    Dispatch(ev);
    return r;
}

bool Hook(uintptr_t at, void *detour, void **orig, const char *name) {
    std::string err;
    bool ok = at && HookAdd(nullptr, at, detour, orig, &err);
    HLOG("game hook %-12s %s: %s", name, DescribeAddress(at).c_str(), ok ? "ok" : err.c_str());
    return ok;
}
} // namespace

// One frame of the loader, on the game thread: mods start at the first one (after the loader lock, before any
// mission), then the bridge's commands, posted calls and the frame event.
void Frame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_last.QuadPart ? static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_freq.QuadPart) : 0.0f;
    g_last = now;
    EnsureCrashFilter();
    if (!g_modsStarted) {
        g_modsStarted = true;
        ModsInit();
    }
    BridgeFrame();
    RunPosted();
    MmlEvent ev = Event(MML_EVENT_FRAME);
    ev.dt = dt;
    Dispatch(ev);
}

const char *GameBuild() { return g_build; }

bool GameHooksInstall() {
    QueryPerformanceFrequency(&g_freq);
    uintptr_t ls3d = ModuleBase("LS3DF.dll");
    if (!ls3d || !game::Detect(ls3d)) {
        HWARN("not a known Mafia build (Game.exe %s, LS3DF.dll %s): no game hooks; the bridge and mods still run",
              game::ExeName(), game::Ls3dName());
        return false;
    }
    g_build = game::ExeName();
    const game::ExeBuild &x = game::Exe();
    Hook(ls3d + game::Ls3d().frameLimit, (void *)hkFrameLimit, (void **)&oFrameLimit, "FrameLimit");
    Hook(x.missionTick, (void *)hkMissionTick, (void **)&oMissionTick, "MissionTick");
    Hook(x.missionClose, (void *)hkMissionClose, (void **)&oMissionClose, "MissionClose");
    Hook(x.actorDtor, (void *)hkActorDtor, (void **)&oActorDtor, "ActorDtor");
    Hook(x.humanCtor, (void *)hkHumanCtor, (void **)&oHumanCtor, "HumanCtor");
    Hook(x.humanHit, (void *)hkHumanHit, (void **)&oHumanHit, "HumanHit");
    return true;
}

} // namespace host
