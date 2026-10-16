// MafiaModLoader host (MafiaModLoader.asi): what every part of the host shares.
//
// The host is the one part that is never unloaded: Ultimate ASI Loader loads it at the game's start and it stays.
// It owns the game's hooks (and turns them into events), the hook chains mods add to, the bridge the MCP server talks
// to, the crash reports, and the mods themselves, each a DLL loaded from a fresh copy so it can be rebuilt and
// reloaded while the game runs. See docs/ARCHITECTURE.md.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "mml/mml.h"

namespace host {

// ---- paths (set once at start)
const std::string &GameDir();   // the game's folder, trailing backslash
const std::string &MmlDir();    // <game>\mml\ (trailing backslash)

// ---- log (log.cpp): <game>\mml\logs\mml.log, the previous run's kept as mml.prev.log
void Log(MmlLogLevel level, const char *who, const char *fmt, ...);
void LogV(MmlLogLevel level, const char *who, const char *fmt, va_list ap);
#define HLOG(...) ::host::Log(MML_LOG_INFO, "host", __VA_ARGS__)
#define HWARN(...) ::host::Log(MML_LOG_WARN, "host", __VA_ARGS__)
std::vector<std::string> LogTail(size_t lines);   // the last lines (any thread)

// ---- memory (memory.cpp): any thread, never faults
bool MemRead(uintptr_t addr, void *dst, size_t n);
bool MemWrite(uintptr_t addr, const void *src, size_t n);
uintptr_t ModuleBase(const char *name);
// "0x5762A0", "5762A0", "game+0x1762A0", "ls3d+1AAE0", "<module>+<hex>"; 0 when it cannot be parsed or resolved.
uintptr_t ParseAddress(const std::string &text);
std::string DescribeAddress(uintptr_t addr);   // "Game.exe+0x1762A0", "LS3DF.dll+0x1AAE0" or the hex value
// One function a module imports pointed at another (its import table's slot), the old one in *old.
bool PatchImport(HMODULE module, const char *dll, const char *fn, void *replacement, void **old);

// ---- crash reports and the main thread (crash.cpp)
void EnsureCrashFilter();      // ours first; called at start and every frame (others may install theirs later)
std::string Where();           // what the game's main thread is doing (any thread)
void SetMainThread(DWORD tid);
DWORD MainThread();

// ---- hook chains (hooks.cpp): several owners may hook one function; owner null: the host
bool HookAdd(MmlMod *owner, uintptr_t target, void *detour, void **original, std::string *err);
bool HookRemove(MmlMod *owner, uintptr_t target);
bool HookRemoveLink(MmlMod *owner, uintptr_t target, void *detour);   // one link only (the host's taps)
void HookRemoveAll(MmlMod *owner);
std::string HookList();        // JSON array

// ---- mods (mods.cpp)
void ModsInit();                          // the host table, the first scan (on the game thread, first frame)
void ModsShutdown();
void Dispatch(const MmlEvent &ev);        // to every subscriber, game thread
void RunPosted();                         // host.post callbacks, game thread
// A command: "mod.reload" etc. (the host's) or a mod's; game thread. ok false: reply is an error message.
bool ExecCommand(const std::string &method, const std::string &paramsJson, std::string *reply);
std::string ModsList();                   // JSON
std::string CommandsList();               // JSON
// For the host's other parts (a mod is its live load; a reload makes a new one):
const char *ModIdOf(const MmlMod *mod);   // "host" for null
bool ModAlive(const MmlMod *mod);
MmlMod *ModById(const std::string &id);   // the live load, or null
bool ModDirById(const std::string &id, std::string *dir);   // any mod found in mml\mods, loaded or not
std::vector<MmlMod *> ModsLoaded();
void DispatchTo(MmlMod *mod, const MmlEvent &ev);            // to one mod's subscribers
// fn(user) called behind the mod's fault guard (a fault: the mod is unloaded at the next frame); false: it faulted.
bool ModCall(MmlMod *mod, void (*fn)(void *), void *user, const char *where);

// ---- a mod's DLLs in the process (lifecycle.cpp): each load's own folder <game>\mml\live\<id>_<pid>_<n>\ (its DLL
// and every other DLL of the mod's folder), the threads still in its code, what the process did not get back
struct ProcessMeasure { double freeMb, largestFreeMb; int handles, threads; };   // address space free, its largest block
ProcessMeasure MeasureProcess();
// The mod's DLL copied to entryPath (inside liveDir), every other *.dll of srcDir next to it (their names in libs).
bool LiveCopy(const std::string &srcDir, const std::string &entry, const std::string &liveDir, const std::string &entryPath,
              std::vector<std::string> *libs, std::string *err);
struct DllDirScope {                      // the process's DLL directory set for a while (a mod's load), then put back
    explicit DllDirScope(const std::string &dir);
    ~DllDirScope();
    std::string previous;
    bool had = false;
};
// libs loaded by their names from somewhere else than liveDir ("name is <path>"): an earlier load not freed, another
// mod's or the game's of the same name
std::vector<std::string> LibsElsewhere(const std::string &liveDir, const std::vector<std::string> &libs);
struct BusyThread { DWORD tid; uintptr_t eip; const char *how; std::string where; };   // how: "at", "returns to", "began at"; where: "<module>+0x<rva>"
std::vector<BusyThread> ThreadsInDir(const std::string &liveDir);   // threads (not this one) in code loaded from there
std::vector<std::string> ModulesInDir(const std::string &liveDir);  // modules still loaded from there
void LiveRemove(const std::string &liveDir);   // its files and itself (what is still mapped stays)
void LiveCleanAll();                      // at start: what earlier runs left in <game>\mml\live\

// ---- game hooks (game_hooks.cpp)
bool GameHooksInstall();                  // false: not a known Mafia build (the loader runs without game events)
void Frame();                             // one loader frame (game thread): from the game's frame limiter, or a pump
const char *GameBuild();                  // "1.2", "1.0", "unknown"
const char *MissionName();                // the open mission's folder name, "" when none
int MissionState();                       // MmlMissionState

// ---- tap (tap.cpp): watch calls to any function, without a mod; the bridge method `tap` {cmd}
namespace tap {
void Init();
std::string Command(const std::string &line);   // "5409D0 n=4 ptr=1:32 log", "show 0", "list", "off all"...
}

// ---- the start and the menus (start.cpp): <game>\mml\start.json; menu.state, menu.command, start.state
void StartInit();                         // after the game's hooks (the build known): reads and deletes start.json
void StartFrame();                        // every frame (game thread): the next step of the start
bool StartMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- input (input.cpp): actions and keys injected inside the game; input.*, text.* methods
void InputInit();                         // after the game's hooks
bool InputMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// Keys and actions for mods (mml.h key_*, action_*, hotkey): as the game got them at its last read.
void InputFrame();                        // every frame (game thread): the hotkeys that went down are called
int KeyCodeOf(const char *name);          // a DirectInput code, -1: no such name
bool KeyDown(int key);
bool KeyPressed(int key);
int ActionSlotOf(const char *name);       // -1: no such action (or no game)
float ActionValue(int slot, bool pressed);
bool HotkeyAdd(MmlMod *mod, const char *combo, MmlHotkeyFn fn, void *user);
void HotkeysRemove(MmlMod *mod);

// ---- drawing (draw.cpp): what mods and the bridge queue, drawn by the game's own 2D drawing in the frame's 2D
// pass; draw.text, draw.rect, draw.line, draw.clear, draw.state
void DrawInit();                          // after the game's hooks
void DrawFrame();                         // every frame (game thread), first: the last frame's items dropped
void DrawText(MmlMod *mod, float x, float y, float size, uint32_t argb, int font, int align, const char *text);
float DrawTextWidth(int font, float size, const char *text);
void DrawLine(MmlMod *mod, float x0, float y0, float x1, float y1, uint32_t argb);
void DrawRect(MmlMod *mod, float x, float y, float w, float h, uint32_t argb, int filled);
void DrawRemove(MmlMod *mod);
bool DrawMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- the game's own menus, extended (menus.cpp): items added to its menus, pages of the mods' own; menu.item,
// menu.items, menu.page, menu.page.close
bool InputClick(float x, float y, std::string *err);   // input.cpp: the menus' cursor to (x, y), a left click
void MenusInit();                         // after the game's hooks
void MenusFrame();                        // every frame: a page that waits for a menu to be open
int MenuItemAdd(MmlMod *mod, const char *menu, int afterId, const char *text, MmlMenuFn fn, void *user);
int MenuPageOpen(MmlMod *mod, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user);
void MenuPageClose(MmlMod *mod);
int MenuPageText(MmlMod *mod, int id, const char *text);
int MenuPageValue(MmlMod *mod, int id, float value);
int MenuPageGetValue(MmlMod *mod, int id, float *value);
int MenuPageRange(MmlMod *mod, int id, int lo, int hi);
int MenuPageGetText(MmlMod *mod, int id, char *buf, int size);
void MenusRemove(MmlMod *mod);
bool MenuControlValue(uintptr_t menu, int id, float *value);   // a slider's or progress bar's value
std::string MenuDefName(uintptr_t menu);  // its definition's name (tables\menu\<name>.mnu), lower case
unsigned GameCodePage();                  // the code page of the game's texts: its language's (1250 Czech, 1252 western)
uintptr_t MenuClassOf(uintptr_t menu);    // the menu's own vtable (the loader may have put its copy in its place)
std::string MenuDescribe(uintptr_t menu); // JSON: its definition's name and its controls
bool MenusMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
void InputTapKey(int key, int frames);    // (input.cpp) a key held for some frames, as input.key holds it

// ---- a mod's files and settings (config.cpp): its data folder, its config.json; config.get, config.set
const char *ModDataDir(MmlMod *mod);
double ConfigNumber(MmlMod *mod, const char *key, double def);
const char *ConfigString(MmlMod *mod, const char *key, const char *def);
const char *ConfigJson(MmlMod *mod, const char *key);
bool ConfigSet(MmlMod *mod, const char *key, const char *valueJson);
void ConfigFrame();                       // every frame: files changed are read again, the mods told
void ConfigForget(MmlMod *mod);           // the mod unloads: what was handed to it is dropped
bool ConfigMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- a mod's own files in the game (files.cpp): rw_data's dtaOpen answered from the mods' files\ folders and
// file_add; files.list, files.which, files.read
void FilesInit();                         // at the process's start (before the game reads its first file)
// A mod's files in the game (its load; initial: the process's start, before any frame, nobody told), or looked at
// again (a reload). Off: taken out (its unload, a failed load).
void FilesModOn(const std::string &id, const std::string &dir, int order, bool initial);
void FilesModOff(const std::string &id);
size_t FilesCount(const std::string &id);
void FilesFrame();                        // every frame: changes on disk looked at, MML_EVENT_FILE_CHANGED sent
int FilesAdd(MmlMod *mod, const char *name, const char *path);
int FilesSource(const char *name, char *path, int size);
int64_t FilesRead(const char *name, void *buf, size_t cap, int flags);
bool FilesMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
void ModsEarlyFiles();                    // (mods.cpp) at the process's start: every enabled mod's files in the game
std::vector<std::string> ModsWantingGfx();   // (mods.cpp) the enabled mods with "gfx": true in their mod.json

// ---- the game's script machine (script.cpp): programs in the missions' language run by the game's interpreter;
// script.run, script.state, script.stop, script.list
int ScriptRun(MmlMod *mod, const char *code, void *owner, void *const *acts, int nacts, const float *flts, int nflts,
              uint32_t timeoutMs, MmlScriptFn fn, void *user, std::string *err);   // a handle, 0: refused (err)
// A program run at once, to its end, in this frame (game thread; no command in it may wait for frames): false with
// err when it does not compile, faults or waits. The registers as it ended in fltOut, actOut (either may be null).
bool ScriptExec(const char *code, void *owner, void *const *acts, int nacts, const float *flts, int nflts,
                std::vector<float> *fltOut, std::vector<void *> *actOut, std::string *err);
void ScriptStop(MmlMod *mod, int handle);
int ScriptState(MmlMod *mod, int handle);
void ScriptsRemove(MmlMod *mod);          // its mod unloads: stopped, not told
void ScriptTick(int ms);                  // after the mission's tick (game thread): one step of each program
void ScriptMissionClose();                // every program stopped
void ScriptActorGone(void *actor);        // out of every program's act[]
bool ScriptMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// An actor named as the bridge names them: "player", "car" (the car the player sits in, else the one he drove
// last), an address ("0x0F892E70": an object with a vtable of the game's).
bool ParseActor(const std::string &text, void **out, std::string *err);

// ---- the world (world.cpp): actors made new by the game's own functions, moved, armed; the game's car list and
// items; world.spawn, world.remove, world.teleport, world.give, world.cars, world.items
void *WorldSpawn(MmlMod *mod, int kind, const char *model, const MmlVec3 *pos, const MmlVec3 *dir, std::string *err);
bool WorldRemove(void *actor, std::string *err);
bool WorldTeleport(void *actor, const MmlVec3 &pos, const MmlVec3 *dir, bool repair, std::string *err);
int WorldItemId(const char *name, std::string *err);      // -1: none (err tells why, with the candidates)
std::string WorldItemName(int id);                         // "" for none
bool WorldGive(void *human, int item, int loaded, int reserve, std::string *err);   // loaded/reserve < 0: the item's own
bool WorldSeat(void *human, void *car, int seat, std::string *err);                 // human_force_settocar
void WorldActorGone(void *actor);         // (game_hooks.cpp) out of the spawned list
void WorldMissionClose();
bool WorldMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// questions to the world (collide.cpp): the game's own ray and sphere tests, the actors near a point, the collision
// grids' shapes; world.ray, world.sweep, world.ground, world.near, world.shapes
int WorldRay(const float from[3], const float to[3], uint32_t what, void *ignore, MmlHit *hit);
int WorldSweep(const float from[3], const float to[3], float radius, uint32_t what, void *ignore, MmlHit *hit);
int WorldNear(const float at[3], float radius, int type, void **out, int max);
int WorldShapes(const float lo[3], const float hi[3], uint32_t what, MmlShape *out, int max);
// the game's state (state.cpp): the freeride's money, a person's health and inventory; state.*, human.items...
int StateMoney();                         // INT32_MIN: no game
bool StateMoneySet(int value, std::string *err);
float StateHealth(void *human, float *max);   // -1: not a person
bool StateHealthSet(void *human, float health, std::string *err);
int StateItems(void *human, MmlItem *out, int max);
bool StateAmmoSet(void *human, int slot, int loaded, int reserve, std::string *err);
bool StateTakeItem(void *human, int item, std::string *err);
// the scene's lights (lights.cpp): LS3D's light frames listed and changed through its setters; lights.list, light.set
void LightsMissionClosed();
int SceneLights(MmlLight *out, int max);
bool LightSet(void *light, const float *color, float power, float rangeNear, float rangeFar, int on, std::string *err);
int SceneSun(float toSun[3], float sunColor[3], float ambient[3]);
bool LightsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
bool StateMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
bool CollideMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- the frame's drawing (render.cpp): I3D_driver::Render hooked; MML_EVENT_RENDER_*, PRESENT; render.state
void RenderInit();                        // after the game's hooks
void RenderStage(int stage);              // (draw.cpp) the scene's callback was called with this stage
void RenderPresent(void *dev8);           // (screenshot.cpp) the device's Present, before the frame is shown
void *RenderDevice(int version);          // mml.h render_device
int RenderCamera(MmlRenderCamera *out);   // mml.h render_camera
bool RenderMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
bool Subscribed(MmlEventType type);       // (mods.cpp) a live mod subscribed to it

// ---- the graphics bridge (gfx.cpp): Direct3D 11 passes of mods over the frame; gfx.state, gfx.reset
void GfxInit();
int GfxPassAdd(MmlMod *mod, int order, MmlGfxPassFn fn, void *user);
void GfxPassRemove(MmlMod *mod, int handle);
bool GfxPassViews(MmlMod *mod, int handle, bool on);   // mml.h gfx_pass_views
void GfxRemove(MmlMod *mod);              // its mod unloads: its passes go
void GfxQuiet(bool on);                   // drawing that is not the game's (not tallied as the world's light)
void GfxBeforeWorld();                    // (render.cpp) the world's Render begins: the readable depth buffer in
void GfxTakeWorldLight();                 // stage 1: the fog and sun the world was drawn with
void GfxStage1();                         // stage 1, after the mods' RENDER_WORLD: copies, passes, composite
void GfxAfterWorld();                     // the world's Render ended: the game's depth buffer back
void GfxUiAsk(MmlMod *mod, int mode);     // mml.h gfx_ui
bool GfxUiBegin();                        // (draw.cpp) the 2D pass begins: true, it draws into the shared target
void GfxUiEnd();                          // ... it ended: the frame's target back (GfxUiBegin said true)
bool GfxMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// scene views' targets (view.cpp draws into them): Begin binds the view's (*w, *h 0: the frame's size, written back;
// clip: a world plane or null), Stage1 takes its linear depth, End puts the frame's target back and hands the view
// to the passes
bool GfxViewBegin(int handle, unsigned *w, unsigned *h, const float *clip, std::string *why);
void GfxViewBackdrop(const float m[16]);  // the view's camera's backdrop (sky) view: no clip plane while it is set
void GfxViewStage1();
void GfxViewEnd(bool drawn, const float view[16], const float proj[16], const float eye[3]);
void GfxViewsNewFrame();                  // a frame of the world begins: no view is fresh yet
void GfxViewForget(int handle);           // the view went: its targets released
bool GfxViewShow(int handle, const float rect[4]);   // view.show: copied onto the frame at its stage 1 (w 0: not)
void GfxViewsShowNow();                   // (render.cpp) the frame's stage 1

// ---- scene views (view.cpp): the mission's scene drawn again by LS3D from mods' cameras; view.state, view.show
int ViewAdd(MmlMod *mod, int w, int h, MmlViewFn fn, void *user, std::string *err);   // mml.h scene_view
void ViewRemove(MmlMod *mod, int handle);   // handle 0: all the mod's
using RenderFn = int(__stdcall *)(void *driver, void *scene);
void ViewsDraw(void *driver, void *scene, RenderFn render);   // (render.cpp) before the world's own Render
void RenderViewWorld(int handle);         // (render.cpp) MML_EVENT_VIEW_WORLD sent, the device's state kept
bool ViewMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
int ViewDrawingNow();                     // the scene view LS3D is drawing now (its handle), 0: none
// the frame's camera overridden (view.cpp): camera_override; camera.state, camera.set, camera.release
int CameraOverride(MmlMod *mod, MmlViewFn fn, void *user);
void FrameCameraBegin(void *scene);       // (render.cpp) before the views and the frame's own drawing
void FrameCameraEnd();                    // ... after the frame's drawing: the game's camera back
int RenderProject(const float world[3], float screen[3]);   // mml.h camera_project (render.cpp)
int RenderRay(float x, float y, float origin[3], float dir[3]);   // mml.h camera_ray
bool CameraMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- what happens (happen.cpp): shots, explosions, cars blown up, deaths, vehicles entered and left
void HappenInit();                        // (game_hooks.cpp) once the build is known
void HappenHit(void *human, void *attacker);
void HappenFrame();                       // every frame of a loaded mission
void HappenMissionClosed();

// ---- people and their skeletons (humans.cpp): humans.list, human.skeleton, human.pose
void HumansCreated(void *human);          // (game_hooks.cpp) C_human's constructor
void HumansGone(void *actor);             // ... an actor's destructor
void HumansMissionClosed();
int HumansList(void **out, int max);      // mml.h humans_list
int HumanSkeleton(void *human, MmlBone *out, int max);
int BoneSet(void *frame, const float *rot, const float *pos);
void *HumanPlayer();                      // mml.h human_player
void HumansBeforeRender();                // (render.cpp) RENDER_BEGIN of the world, before the mods: the held poses
void HumansFlushPoses();                  // ... after the mods' RENDER_BEGIN: the posed models updated once each
bool HumansMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- hardware watchpoints on the game thread (watch.cpp): watch.set, watch.show, watch.clear
bool WatchMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- native.call (native.cpp): any function called from the bridge by its address and convention
bool NativeMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- what happens, kept for agents (events.cpp): events.since on the bridge's thread, MCP's `wait`
void EventsRecord(const char *type, const std::string &dataJson);   // a host's own event (game thread)
void EventsFromDispatch(const MmlEvent &ev);                         // (mods.cpp Dispatch) every event but each frame's

// ---- measuring the frame (prof.cpp): each entry into a mod timed by its site, the host's parts, the frames, a
// sampling profiler of the game thread; prof.state, prof.reset, prof.sample
void ProfEnter(const MmlMod *mod, const char *site);
void ProfLeave(const MmlMod *mod, const char *site, double ms);
double ProfNow();
const char *ProfEventSite(int type);      // an event's name ("RENDER_WORLD")
void ProfPhase(int phase);                // what the game thread does now, for the sampler (1 drawing the frame, 2 the
                                          // scene views, 3 the graphics bridge); 0: back to the one before
void ProfFrame();                         // (render.cpp) Present: a frame complete
bool ProfFrameStats(const MmlMod *mod, MmlFrameStats *out);   // mml.h frame_stats
bool ProfMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
struct ProfScope {                        // times a call into a mod (mod null: a part of the host's own)
    const MmlMod *mod;
    const char *site;
    double t0;
    ProfScope(const MmlMod *m, const char *s) : mod(m), site(s), t0(ProfNow()) { ProfEnter(mod, site); }
    ~ProfScope() { ProfLeave(mod, site, ProfNow() - t0); }
    ProfScope(const ProfScope &) = delete;
    ProfScope &operator=(const ProfScope &) = delete;
};
struct ProfPhaseScope {
    explicit ProfPhaseScope(int phase) { ProfPhase(phase); }
    ~ProfPhaseScope() { ProfPhase(0); }
};

// ---- the game's own geometry (geom.cpp): face groups left out of LS3D's drawing, surveyed, captured by texture
int GeomHide(MmlMod *mod, const char *pattern, int where, std::string *err);   // mml.h geom_hide
void GeomShow(MmlMod *mod, int handle);   // handle 0: all the mod's
bool GeomCapture(MmlMod *mod, const char *pattern, MmlGeomFn fn, void *user, std::string *err);
void GeomRemove(MmlMod *mod);             // its mod unloads
void GeomFrameBegin();                    // (render.cpp) a frame of the mission's world: before its scene views
void GeomFrameDrawing(bool on);           // ... the frame's own Render drawing
void GeomFrameEnd();                      // ... its end: captures and surveys done
bool GeomMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- screenshots (screenshot.cpp): the back buffer at Present; screenshot, screenshot.state
bool ScreenshotHookPresent(std::string *err);   // the device's Present hooked (once; err may be null)
bool ScreenshotMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- bridge (bridge.cpp)
void BridgeStart();
void BridgeFrame();                       // run the commands waiting for the game thread
uint64_t Frames();

} // namespace host
