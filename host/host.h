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

// ---- screenshots (screenshot.cpp): the back buffer at Present; screenshot, screenshot.state
bool ScreenshotMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- bridge (bridge.cpp)
void BridgeStart();
void BridgeFrame();                       // run the commands waiting for the game thread
uint64_t Frames();

} // namespace host
