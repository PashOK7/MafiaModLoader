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
using MissionOpenFn = int(__fastcall *)(void *, void *, const char *, int, unsigned, int);   // thiscall, ret 0x10
MissionOpenFn oMissionOpen;

// The mission, as mods are told of it (mml.h MML_EVENT_MISSION_OPEN, _LOADED, MML_EVENT_GAME_PAUSED, _RESUMED).
// C_mission::Open closes the open mission first (MISSION_CLOSE), then loads the new one: MISSION_OPEN is sent
// between the two, from the close's hook, so mods see the old world go before the new one's name. The loading is
// done when the mission ticks for the first time. A pause: a menu is open and the game's loop has stopped calling
// the mission's tick (the pause menu's loop does not; the main menu's does: its mission, 00menu, runs under it).
std::string g_missionName, g_openName;
bool g_inOpen, g_openSent, g_missionLoaded, g_paused;
unsigned g_ticks, g_ticksSeen;
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
// Its answer, whole milliseconds, is the step every system of the game is given (the mission's tick, the scene's
// animations, the physics): scaled by state.time (0: the world stands still while it is drawn), the fraction carried
// to the next frame.
double g_timeScale = 1, g_timeCarry;
int g_lastStep, g_lastRealStep;
int __stdcall hkFrameLimit(void *driver, int a, int b) {
    int r = oFrameLimit(driver, a, b);
    g_lastRealStep = r;
    if (g_timeScale != 1) {
        g_timeCarry += r * g_timeScale;
        r = static_cast<int>(g_timeCarry);
        g_timeCarry -= r;
    }
    g_lastStep = r;
    Frame();
    return r;
}
} // namespace
void GameTimeScaleSet(double scale) {
    g_timeScale = scale;
    g_timeCarry = 0;
}
double GameTimeScale(int *step, int *real) {
    if (step) *step = g_lastStep;
    if (real) *real = g_lastRealStep;
    return g_timeScale;
}
namespace {
void SendNamed(MmlEventType type) {
    std::string name = g_missionName;   // (a handler may open a mission)
    MmlEvent ev = Event(type);
    ev.name = name.c_str();
    Dispatch(ev);
}
void SendOpen() {
    g_openSent = true;
    g_missionName = g_openName;
    g_missionLoaded = false;
    HLOG("mission '%s' opens", g_missionName.c_str());
    SendNamed(MML_EVENT_MISSION_OPEN);
}
void SetPaused(bool paused) {
    if (paused == g_paused) return;
    g_paused = paused;
    Dispatch(Event(paused ? MML_EVENT_GAME_PAUSED : MML_EVENT_GAME_RESUMED));
}

int __fastcall hkMissionOpen(void *self, void *edx, const char *name, int a, unsigned b, int c) {
    char safe[128] = {};
    if (name) MemRead(reinterpret_cast<uintptr_t>(name), safe, sizeof safe - 1);
    g_openName = safe;
    g_inOpen = true;
    g_openSent = false;
    int r = oMissionOpen(self, edx, name, a, b, c);
    if (!g_openSent) SendOpen();   // (Open did not close anything first)
    g_inOpen = false;
    return r;
}
int __fastcall hkMissionTick(void *self, void *edx, int ms) {
    // a step of 0 (state.time below 1: the world held, or a slowed frame's whole millisecond not yet made) is no tick
    // at all, as in the game's pause: a car's physics takes 0 ms as 1 (C_Vehicle::Tick), so the traffic would roll on
    // at 1 ms a frame. Its time comes with the next step (the fraction carried). The game's loop reads no result.
    if (ms == 0 && g_timeScale != 1) return 0;
    int r = oMissionTick(self, edx, ms);
    g_ticks++;
    if (!g_missionLoaded && !g_missionName.empty()) {
        g_missionLoaded = true;
        HLOG("mission '%s' loaded", g_missionName.c_str());
        SendNamed(MML_EVENT_MISSION_LOADED);
    }
    SetPaused(false);
    MmlEvent ev = Event(MML_EVENT_MISSION_TICK);
    ev.dt = ms > 0 && ms < 200 ? ms / 1000.0f : 1.0f / 60.0f;
    Dispatch(ev);
    SidecarTick(ev.dt);   // (the 64-bit halves subscribed to it)
    ScriptTick(ms);   // the programs of agents and mods, after the mission's own
    OrdersTick(ms);   // the people's orders done: ORDER_ENDED (a retry's wait in the mission's time)
    InteractTick(ms);   // the timers on the mission's clock
    return r;
}
int __fastcall hkMissionClose(void *self, void *edx) {
    ScriptMissionClose();   // (their actors go with the mission)
    OrdersMissionClose();   // (the orders' frames)
    WorldMissionClose();
    PropsMissionClose();    // (the props unlinked and released while their scene is there)
    MeshesMissionClose();   // (the scene's frames and meshes forgotten)
    SkinMissionClose();     // (the single meshes bound to the mods' skins)
    VdataMissionClose();    // (the rules' vertex data: the pools' places given anew)
    ShadowsMissionClose();  // (the shadow pass's buffers of those meshes)
    GameCameraMissionClose();   // (a held field of view: the next mission's camera is the game's)
    SoundMissionClose();    // (the 3D sounds, likewise)
    ControlMissionClose();
    InteractMissionClose();
    HudMissionClose();
    StateMissionClose();      // (the invulnerable people go with the mission)
    AnimsMissionClose();
    ScaleMissionClose();
    HitsMissionClose();       // (the bullets' shapes, the bridge's takes)      // (the radar's marks and the compass given back while the HUD knows the mission's frames)
    SetPaused(false);
    Dispatch(Event(MML_EVENT_MISSION_CLOSE));
    g_missionLoaded = false;
    g_missionName.clear();
    int r = oMissionClose(self, edx);
    HumansMissionClosed();   // (its people went with it)
    LightsMissionClosed();   // (its lights too)
    RenderMissionClosed();   // (the old scene's original camera range)
    EffectsMissionClosed();
    HappenMissionClosed();
    BatchMissionClosed();   // (its visuals go with it)
    if (g_inOpen && !g_openSent) SendOpen();
    return r;
}
void __fastcall hkActorDtor(void *self, void *edx) {
    MmlEvent ev = Event(MML_EVENT_ACTOR_DESTROYED);
    ev.actor = self;
    Dispatch(ev);
    HumansGone(self);
    ScriptActorGone(self);
    OrdersActorGone(self);
    GameCameraActorGone(self);
    WorldActorGone(self);
    PropsActorGone(self);
    InteractActorGone(self);
    HudActorGone(self);
    StateActorGone(self);
    AnimsActorGone(self);
    HitsActorGone(self);
    ScaleActorGone(self);
    CameraFollowGone(self);
    oActorDtor(self, edx);
}
void *__fastcall hkHumanCtor(void *self, void *edx) {
    void *r = oHumanCtor(self, edx);
    HumansCreated(self);
    MmlEvent ev = Event(MML_EVENT_HUMAN_CREATED);
    ev.actor = self;
    Dispatch(ev);
    return r;
}
int __fastcall hkHumanHit(void *self, void *edx, int type, MmlVec3 *dir, MmlVec3 *pos, MmlVec3 *normal, float dmg, void *attacker,
                          unsigned part, void *frame) {
    // a person kept from harm (state.cpp human_invulnerable): the game's Hit not run for him, as C_player::Hit skips it
    // while a scene holds the player (no damage, no reaction, no event: nothing happened to him)
    if (StateInvulnerable(self)) return 0;
    uint8_t before = 0, after = 0;
    MemRead(reinterpret_cast<uintptr_t>(self) + game::kActorAlive, &before, 1);
    MmlHumanHit hit{};
    hit.type = type;
    if (dir) hit.dir = *dir;
    if (pos) hit.pos = *pos;
    hit.damage = dmg;
    hit.attacker = attacker;
    hit.part = static_cast<int>(part);
    hit.shape = HitsShapeOf(self);   // (a bullet the mods' shapes gave him: hits.cpp)
    hit.was_alive = hit.alive = before != 0;
    // the mods' filters first (hits.cpp): one may take the hit (the game's Hit then does not run) or change it
    if (HitsFilter(self, 0, &hit)) {
        MmlEvent ev = Event(MML_EVENT_HUMAN_HIT);
        ev.actor = self;
        ev.hit = &hit;
        Dispatch(ev);
        return 0;
    }
    MmlVec3 d = hit.dir, at = hit.pos;
    int r = oHumanHit(self, edx, hit.type, dir ? &d : nullptr, pos ? &at : nullptr, normal, hit.damage, hit.attacker,
                      static_cast<unsigned>(hit.part), frame);
    MemRead(reinterpret_cast<uintptr_t>(self) + game::kActorAlive, &after, 1);
    hit.alive = after != 0;
    attacker = hit.attacker;
    HappenHit(self, attacker);
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
    FpuFrameEnter();   // (fpu.cpp: the game's x87 word, as it comes in)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_last.QuadPart ? static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_freq.QuadPart) : 0.0f;
    g_last = now;
    CrashFrame();
    AsiCompatFrame();
    if (!g_modsStarted) {
        g_modsStarted = true;
        ModsInit();
    }
    DrawFrame();
    BridgeFrame();
    RunPosted();
    StartFrame();
    MenusFrame();
    PanelFrame();     // (panel.cpp: a key taken for a mod's setting)
    InputFrame();     // (before the settings' events: a handler that binds its hotkey again must not lose a press)
    ConfigFrame();
    SettingsFrame();  // (the loader's own settings changed: applied)
    FilesFrame();
    SoundFrame();
    VideoFrame();     // (video.cpp: video.play's video, played from the frame's own call)
    // a pause: a frame went by without the mission's tick while a menu is open over a loaded mission
    bool ticked = g_ticks != g_ticksSeen;
    g_ticksSeen = g_ticks;
    if (g_missionLoaded && !ticked && game::g_exe && game::Exe().menuTop) {
        uintptr_t menu = 0;
        MemRead(game::Exe().menuTop, &menu, sizeof menu);
        if (menu) SetPaused(true);
    }
    if (g_missionLoaded) HappenFrame();   // (happen.cpp: deaths, vehicles entered and left)
    if (g_missionLoaded) InteractFrame();   // (interact.cpp: zones crossed, use-points)
    if (g_missionLoaded) StatsFrame(dt, ticked);   // (stats.cpp: the time played, the distances)
    if (g_missionLoaded) PropsFrame();    // (props.cpp: physics asked again once the one before is destroyed)
    if (g_missionLoaded) MeshesFrame();   // (meshes.cpp: the scene walked and scanned around the mods' boxes)
    ItemsFrame();   // (items.cpp: the mods' items in the game's table)
    MmlEvent ev = Event(MML_EVENT_FRAME);
    ev.dt = dt;
    Dispatch(ev);
    SidecarFrame(dt);
    FpuFrameExit();    // (and as it goes back: the game's again)
}

const char *GameBuild() { return g_build; }
const char *MissionName() { return g_missionName.c_str(); }
int MissionState() {
    if (g_missionName.empty()) return MML_MISSION_NONE;
    if (!g_missionLoaded) return MML_MISSION_LOADING;
    return g_paused ? MML_MISSION_PAUSED : MML_MISSION_RUNNING;
}

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
    if (x.missionOpen) Hook(x.missionOpen, (void *)hkMissionOpen, (void **)&oMissionOpen, "MissionOpen");
    Hook(x.actorDtor, (void *)hkActorDtor, (void **)&oActorDtor, "ActorDtor");
    Hook(x.humanCtor, (void *)hkHumanCtor, (void **)&oHumanCtor, "HumanCtor");
    Hook(x.humanHit, (void *)hkHumanHit, (void **)&oHumanHit, "HumanHit");
    HappenInit();   // (happen.cpp: shots, explosions, cars blown up)
    SkinStart();    // (skin.cpp: the single meshes' skinning, LS3D's own unless a mod gives a model's)
    return true;
}

} // namespace host
