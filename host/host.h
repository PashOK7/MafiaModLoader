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
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "mml/mml.h"

struct IDirect3DBaseTexture9;   // (d3d9.h's: shadows.cpp's map)

namespace host {

// ---- paths (set once at start). Every path the host keeps is UTF-8: the bridge's JSON, the log, the mods' ABI
// (game_dir, mod_dir, mod_data_dir). Windows is asked through its wide (W) functions, here (paths.cpp), so a game
// in a folder of any name works whatever the system's code page (an ANSI path once threw in json's dump at the
// start, in a Cyrillic folder: 2026-10-17). The game's own names are in the system's code page (CP_ACP): AcpPath
// gives a path to the game's ANSI functions (rw_data's dtaOpen), AcpToUtf8 takes one from it.
const std::string &GameDir();   // the game's folder, trailing backslash
const std::string &MmlDir();    // <game>\mml\ (trailing backslash)
bool OutOfProcess();            // loaded into the game's Setup.exe: nothing started (main.cpp)
void GammaDevice(void *dev8);   // (gfx.cpp) the game's device made: its gamma ramp followed on DXVK (gamma.cpp)
void GammaReset();              // ... before its Reset
void GammaPresent(void *dev8);  // (screenshot.cpp) at its Present: the ramp drawn over the frame in a window
std::string GammaState();       // for gfx.state
// (fullscreen.cpp) Setup's full screen made a borderless window while a mod has "gfx": true
void FullscreenCreate(void *d3d8, UINT adapter, HWND focus, DWORD *pp);   // (gfx.cpp) before CreateDevice: Direct3D 8's parameters
void FullscreenReset(DWORD *pp);                                          // ... before the device's Reset
const RECT *FullscreenDest();   // the picture's place in the window (Present's destination), or null: the game's own
void FullscreenPresent();       // (screenshot.cpp) at the game's Present: the bars beside the picture
std::string FullscreenState();  // for gfx.state
std::wstring Wide(const std::string &utf8);
std::string Utf8Of(const wchar_t *wide, int n = -1);   // n: characters, -1: up to the terminator
std::string AcpToUtf8(const char *acp);
std::string AcpOf(const std::string &utf8);   // a name (not a file on disk) in the system's code page, best guess
// For the game's ANSI functions: the path in the system's code page when it spells every character, else its short
// (8.3) form (ASCII when the volume makes short names; the file must exist), else the code page's best guess
// (*spelled false: an ANSI function cannot open it by that name).
std::string AcpPath(const std::string &utf8, bool *spelled = nullptr);
std::string ModulePath(HMODULE module);   // a loaded module's file, "" when unknown
FILE *FileOpen(const std::string &path, const char *mode);   // _wfopen
DWORD PathAttributes(const std::string &path);               // INVALID_FILE_ATTRIBUTES when it is not there
bool PathExists(const std::string &path);
bool PathAttributesEx(const std::string &path, WIN32_FILE_ATTRIBUTE_DATA *out);
bool MakeDir(const std::string &path);                       // true: made, or it was there
bool RemoveDir(const std::string &path);
bool PathDelete(const std::string &path);
bool PathMove(const std::string &from, const std::string &to, DWORD flags = 0);   // MoveFileExW's flags
bool PathCopy(const std::string &from, const std::string &to, bool failIfExists);
HANDLE PathCreateFile(const std::string &path, DWORD access, DWORD share, DWORD disposition, DWORD flags);
HMODULE PathLoadLibrary(const std::string &path, DWORD flags);
struct FoundFile {
    std::string name;           // UTF-8
    DWORD attributes;
    uint64_t size;
    FILETIME written;
    bool dir() const { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};
std::vector<FoundFile> ListDir(const std::string &pattern);  // FindFirstFileW's matches ("dir\\*.dll"), no "." or ".."

// ---- log (log.cpp): <game>\mml\logs\mml.log, the previous run's kept as mml.prev.log
void Log(MmlLogLevel level, const char *who, const char *fmt, ...);
void LogV(MmlLogLevel level, const char *who, const char *fmt, va_list ap);
#define HLOG(...) ::host::Log(MML_LOG_INFO, "host", __VA_ARGS__)
#define HWARN(...) ::host::Log(MML_LOG_WARN, "host", __VA_ARGS__)
std::vector<std::string> LogTail(size_t lines);   // the last lines (any thread)
void GatherStart();   // (gather.cpp) the other parts' logs (DXVK's, NGX's, LS3D's log.txt...) read into this one as they grow
void GatherFinal();   // ... once more: the process ends or crashed
void LogQuiet();                                  // no log file from now on (main.cpp: a second game of a folder)

// ---- memory (memory.cpp): any thread, never faults
bool MemRead(uintptr_t addr, void *dst, size_t n);
bool MemWrite(uintptr_t addr, const void *src, size_t n);
uintptr_t ModuleBase(const char *name);
// The game's address space (a 32-bit process: 2 GB, or 4 GB on 64-bit Windows when Game.exe is Large Address Aware,
// "the 4 GB patch"): what is free and its largest piece. A texture pack, DXVK, ReShade and the bridge together once
// left the game no 16 MB for a texture (an operator new that returned null in ITexture::Create).
struct AddressSpace { unsigned totalMb, freeMb, largestMb; bool largeAddressAware; };
AddressSpace AddressSpaceNow();
// "0x5762A0", "5762A0", "game+0x1762A0", "ls3d+1AAE0", "<module>+<hex>"; 0 when it cannot be parsed or resolved.
uintptr_t ParseAddress(const std::string &text);
std::string DescribeAddress(uintptr_t addr);   // "Game.exe+0x1762A0", "LS3DF.dll+0x1AAE0" or the hex value
// One function a module imports pointed at another (its import table's slot), the old one in *old.
bool PatchImport(HMODULE module, const char *dll, const char *fn, void *replacement, void **old);

// ---- crash reports and the main thread (crash.cpp)
void EnsureCrashFilter();      // ours set first (chaining to what was there)
void CrashFilterStart();       // at start: ours set, SetUnhandledExceptionFilter hooked (another's goes after ours)
void CrashFrame();             // each frame: ours set again only without the hook; another's set, logged
bool CrashMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);   // crash.filter
// The game's time scaled (game_hooks.cpp, the frame limiter's step: every system's): 0 the world stands still while
// it is drawn; `step` and `real` the last frame's given and real steps (ms). state.time.
void GameTimeScaleSet(double scale);
double GameTimeScale(int *step, int *real);
std::string Where();           // what the game's main thread is doing (any thread)
std::string ExitStack();       // who is ending the process: the calls on this thread's stack (DllMain's detach)
// ---- a thread's calls read from its stack (stack.cpp: the sampler's and the crash report's one walk)
enum CodeKind : uint8_t { kCodeNone, kCodeOther, kCodeGame, kCodeOurs };   // ours: the loader and the mods (mml\live)
using CodeKindFn = CodeKind (*)(uintptr_t addr, void *ctx);   // whose code an address is (kCodeNone: not code)
// A stack as read: the thread's registers and its stack from esp (n dwords: the thread's own, or a copy of it).
struct StackView { uint32_t eip, esp, ebp; const uint32_t *stack; size_t n; };
// A return address and its call's target (CallTarget; 0: an indirect call; kCallWalked: found by the frame pointers)
struct StackCall { uint32_t ret, target; };
constexpr uint32_t kCallWalked = 0xFFFFFFFEu;
bool AfterCallAt(uintptr_t a);       // right after a call instruction
uint32_t CallTarget(uint32_t ret);   // the call's target, followed to the code that runs; 0 when it cannot be read
// The calls on a stack, top first: walked by the frame pointers through the system's and our frames up to the first
// of the game's (certain; the system's in `out` only with `others`), then every return address into the game's or our
// code after it, each with its call's target. The count (at most cap).
int StackCalls(const StackView &v, CodeKindFn kind, void *ctx, bool others, StackCall *out, int cap);
// The same for this thread from a context of its own (the faulting thread's, in a filter): its live stack, the
// modules asked as it goes (no TLS, no snapshot: any thread, any time).
int StackCallsOf(const CONTEXT *c, bool others, StackCall *out, int cap);
// The host's rough check of a chain (MCP checks it exactly against the functions' starts: crash_triage, profile):
// eip, then each return address whose direct call's target is within 32 KiB below a frame kept just before it.
int LikelyFrames(uint32_t eip, const StackCall *calls, int n, uintptr_t *out, int cap);
// A thread's likely call stack from a context of its own: StackCallsOf, then LikelyFrames; the count.
int StackOf(const CONTEXT *c, uintptr_t *out, int cap);
void SetMainThread(DWORD tid);
DWORD MainThread();
// Whether this thread has a TLS vector (the TEB's ThreadLocalStoragePointer, fs:[0x2C]): a thread made with
// NtCreateThreadEx's SkipThreadAttach (injectors, overlays: one ran in a player's game) has none, and `thread_local`
// read on it faults at null. Code any thread may run (the crash filter, a tap's thunks) asks before using one.
inline bool ThreadHasTls() { return __readfsdword(0x2C) != 0; }

// ---- hook chains (hooks.cpp): several owners may hook one function; owner null: the host
bool HookAdd(MmlMod *owner, uintptr_t target, void *detour, void **original, std::string *err);
// A link kept outermost whatever is added after it (the host's counters: the caller's own return address is theirs),
// or innermost (the time past every other hook: the game's own function's, the driver's)
bool HookAddOuter(uintptr_t target, void *detour, void **original, std::string *err);
bool HookAddInner(uintptr_t target, void *detour, void **original, std::string *err);
bool HookRemove(MmlMod *owner, uintptr_t target);
bool HookRemoveLink(MmlMod *owner, uintptr_t target, void *detour);   // one link only (the host's taps)
void HookRemoveAll(MmlMod *owner);
bool HookMid(MmlMod *owner, uintptr_t target, MmlMidFn fn, void *user, std::string *err);   // a link that calls fn
                                                    // with the registers, anywhere in a function (mml.h hook_mid)
bool PatchAdd(MmlMod *owner, uintptr_t at, const void *expect, const void *bytes, size_t n, std::string *err);   // code_patch
bool PatchRemove(MmlMod *owner, uintptr_t at);      // its bytes put back (unless changed since: left, logged)
void PatchRemoveAll(MmlMod *owner);                 // at the mod's unload
std::string PatchList();                            // patches.list
std::string HookList();        // JSON array
// The function a MinHook trampoline of a chain begins (a detour's call of the original), 0 for any other address;
// lock-free (the sampler, a crash's filter)
uintptr_t HookedByTrampoline(uintptr_t at);

// ---- mods (mods.cpp)
void ModsInit();                          // the host table, the first scan (on the game thread, first frame)
void ModsShutdown();
void Dispatch(const MmlEvent &ev);        // to every subscriber, game thread
void RunPosted();                         // host.post callbacks, game thread
// A command: "mod.reload" etc. (the host's) or a mod's; game thread. ok false: reply is an error message.
bool ExecCommand(const std::string &method, const std::string &paramsJson, std::string *reply);
std::string ModsList();                   // JSON
std::string ModAt(uintptr_t addr);        // the mod whose code holds an address ("hook_b", "hook_b, an old load"), "" for none
std::string CommandsList();               // JSON
// For the host's other parts (a mod is its live load; a reload makes a new one):
const char *ModIdOf(const MmlMod *mod);   // "host" for null
bool ModAlive(const MmlMod *mod);
MmlMod *ModById(const std::string &id);   // the live load, or null
bool ModDirById(const std::string &id, std::string *dir);   // any mod found in mml\mods, loaded or not
std::vector<MmlMod *> ModsLoaded();
std::string ModSettingsJson(const MmlMod *mod);   // its manifest's "settings" (panel.cpp), "": none
std::string ModNameOf(const MmlMod *mod);
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
// a key taken for a setting (panel.cpp): started, stopped, its result (0 waiting, 1 taken as a combo, -1 Esc)
void KeyGrabStart();
void KeyGrabStop();
int KeyGrabResult(std::string *combo);
bool KeyGrabbing();
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
void DrawPicture(MmlMod *mod, int picture, float x, float y, float w, float h, float angle, uint32_t argb, const float uv[4]);
// the host's items for the 2D pass being drawn (hud.cpp's markers, queued from HudPass): under the owner's guard
void DrawPassText(MmlMod *owner, float x, float y, float size, uint32_t argb, int font, int align, const char *text);
void DrawPassPicture(MmlMod *owner, int picture, float x, float y, float w, float h, float angle, uint32_t argb);
void DrawPassQuad(MmlMod *owner, const float xy[8], uint32_t argb);   // a filled quad, a triangle strip's corners
bool DrawUnitsFromScreen(float x, float y, float *ux, float *uy);     // camera_project's 800x600 to the HUD's units
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
bool MenuPageIsOpen(MmlMod *mod);         // the owner's page is made or waits to be (1.2: until the game deletes it)
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
// panel.cpp's: a change made live (the mod told at the next frame, the file written once the changes stop; "null":
// back to the default), written now, a key's default and its value now (JSON text, "": none)
bool ConfigSetLive(MmlMod *mod, const char *key, const std::string &valueJson);
void ConfigFlush(MmlMod *mod);
std::string ConfigDefaultJson(MmlMod *mod, const char *key);
std::string ConfigValueJson(MmlMod *mod, const char *key);
// the x87's precision around the mods' code (fpu.cpp): FpuDouble sets 53 bits (when it was 24) and returns the word
// it found, FpuRestore gives it back; the game's own word as it entered the loader's frame
uint16_t FpuWord();
void FpuSet(uint16_t w);
uint16_t FpuDouble();
void FpuRestore(uint16_t w);
uint16_t FpuGameWord();
void FpuFrameEnter();
void FpuFrameExit();
bool FpuMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// the game's own camera logic (gamecam.cpp): mml.h game_camera, game_camera_mode, game_camera_fov
int GameCamera(MmlGameCamera *out);
int GameCameraMode(MmlMod *mod, int mode);
int GameCameraFov(MmlMod *mod, float fov);
void GameCameraFrame();                    // (render.cpp) a held field of view put back on the game's camera
void GameCameraModGone(MmlMod *mod);
void GameCameraMissionClose();
void GameCameraActorGone(void *actor);    // (an actor's destructor) a followed car: the camera back on the player
bool GameCameraMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// the 64-bit sidecar (sidecar.cpp; sidecar/main.cpp the other side; sdk/include/mml/side.h)
bool SidecarStart(std::string *err);
void SidecarModLoad(const std::string &id, const std::string &dir, const std::string &entry64, const std::string &dataDir, unsigned generation);
void SidecarModUnload(const std::string &id, const char *why);
void SidecarEvent(const char *type, const std::string &dataJson);   // (any thread) to the halves subscribed to it
void SidecarTick(float dt);
void SidecarFrame(float dt);
void SidecarAdapter(uint32_t luidLow, uint32_t luidHigh);         // the adapter the game draws on
bool SidecarCommand(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);   // (bridge thread)
std::string SidecarState();                       // sidecar.state's answer (JSON)
std::string SidecarHalf(const std::string &id);   // a mod's 64-bit half in mods.list (JSON, "" none)
std::vector<std::pair<std::string, std::string>> SidecarCommands();
// A half's shared texture by name: its size, format, the half's generation and the loader's stamp (a new one each time
// the half makes it anew); with dup, handle is this process's own copy of its handle (the caller closes it).
struct SidecarTex { void *handle; uint32_t w, h, format, generation; uint64_t stamp; };
bool SidecarTexture(const std::string &mod, const std::string &name, SidecarTex *out, bool dup);
struct SidecarFenceInfo { void *handle; uint32_t generation; uint64_t stamp; };   // a half's shared fence, as SidecarTex
bool SidecarFence(const std::string &mod, const std::string &name, SidecarFenceInfo *out, bool dup);
// the channels of the mods' 32-bit halves (mml.h channel_open; the memory sidecar/channel.h describes); any thread
MmlChannel *SidecarChannelOpen(MmlMod *mod, const char *name, uint32_t size, std::string *err);
int SidecarChannelWrite(MmlChannel *ch, const void *data, uint32_t bytes);
uint32_t SidecarChannelRead(MmlChannel *ch, void *data, uint32_t cap, uint64_t *version);
uint64_t SidecarChannelWait(MmlChannel *ch, uint64_t after, uint32_t timeoutMs);
void SidecarChannelsForget(MmlMod *mod);   // (the mod's unload) its channels closed
int GfxSharedTexture(MmlMod *mod, const char *name, MmlSharedTexture *out);   // (gfx.cpp; inside a graphics pass)
int GfxSharedFence(MmlMod *mod, const char *name, MmlSharedFence *out);       // (gfx.cpp; inside a graphics pass)
int GfxShowSet(MmlMod *mod, MmlGfxShowFn fn, void *user);                     // (gfx.cpp) gfx_show
int GfxJitterSet(MmlMod *mod, int on, float x, float y);                       // (gfx.cpp) gfx_jitter
// The game's Present (screenshot.cpp's hook): present() shows the back buffer; a mod's pictures (gfx_show) go first.
HRESULT GfxShow(void *dev8, const std::function<HRESULT()> &present);
bool SidecarWants(const char *type);
void SidecarShutdown();
bool SidecarMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// the mods' settings in the game's Options > Mods (panel.cpp)
void PanelInit();
void PanelFrame();                        // (game_hooks.cpp) a key taken for a "key" row made the setting
bool PanelMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
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
void FilesNames(std::vector<std::pair<std::string, std::string>> *out);   // every name the mods answer (lower case), its mod
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
// An order's code compiled into a program already made (a C_entity's own, +0xA9C): its old instructions and
// registers freed by its Done, `prefix` a line before the code (line 0), the registers given (frames in frm[]), pc 0.
bool ScriptCompileInto(uintptr_t prog, const char *prefix, const char *code, void *const *acts, int nacts, const float *flts,
                       int nflts, void *const *frms, int nfrms, std::vector<int> *lines, std::string *err);
void ScriptWhere(uintptr_t prog, unsigned *pc, unsigned *count, bool *atEnd);

// ---- orders for people (orders.cpp): a scripted person's (C_entity's) own program given an order in its own
// commands (ENEMY_*), run by the game's AI; human.order, human.order_state, human.go, human.follow, human.attack,
// human.flee, human.enter, human.stop
int HumanOrder(MmlMod *mod, void *human, const char *code, void *const *acts, int nacts, const float *flts, int nflts,
               const float *points, int npoints, std::string *err);   // its id, 0: refused (err)
int HumanOrderState(void *human, MmlOrderState *out);                 // 1: an order of the loader's is known for him
void OrdersTick(int ms);                        // (game_hooks.cpp) after the mission's tick: ORDER_ENDED for those done
void OrdersActorGone(void *actor);        // ... an actor's destructor
void OrdersMissionClose();
bool OrdersMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// An LS3D dummy frame of the loader's at a place (hud.cpp), moved, released.
void *DummyFrameAt(const float pos[3]);
void DummyFrameMove(void *frame, const float pos[3]);
void FrameRelease(void *frame);
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
int RoadNear(const float at[3], float out[3]);   // the road network's node nearest (its index, out its place); -1 none
int WebNodes(const float at[3], float radius, MmlWebNode *out, int max);   // the pedestrians' web near a point (how many)
const char *MaterialName(int id);         // (collide.cpp) a surface's name in the game's table, UTF-8; null none
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
bool StateInvulnerable(void *human);      // (C_human::Hit's hook) kept from the game's harm now
int StateInvulnerableSet(MmlMod *mod, void *human, int on, std::string *err);   // on 1, 0, -1 asks; his state, -1 not a person
void StateActorGone(void *actor);
void StateMissionClose();
void StateModGone(MmlMod *mod);
// the game's effects (effects.cpp): the car exhaust read and hooked, tables\particles.def read and written
int CarExhaust(void *car, MmlExhaust *out);
int ExhaustHook(MmlMod *mod, MmlExhaustFn fn, void *user);
void EffectsModGone(MmlMod *mod);         // (its unload) its exhaust and particle hooks taken out
void EffectsMissionClosed();              // the particles' tracks forgotten
int ParticleHook(MmlMod *mod, MmlParticleFn fn, void *user);
int WeatherGet(int param, double *value);
int WeatherSet(int param, double value);
int WeatherReset();
int ParticleDef(int index, MmlParticleDef *out);   // index -1: the count
int ParticleDefSet(int index, const MmlParticleDef *def);
bool EffectsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// props (props.cpp): models in the world without actors, made, placed, attached, shown, removed
void *PropCreate(MmlMod *mod, const char *model, void *parent, const float *pos, const float *rot, float scale, std::string *err);
bool PropPlace(void *prop, const float *pos, const float *rot, float scale, std::string *err);
bool PropAttach(void *prop, void *parent, bool keepWorld, std::string *err);
bool PropShow(void *prop, bool on, std::string *err);
bool PropRemove(void *prop, std::string *err);
void PropsModGone(MmlMod *mod);           // (its unload) its props removed
void PropsMissionClose();                 // (before the mission closes) every prop removed
void PropsActorGone(void *actor);         // (an actor's destructor) props on it put into the world
int PropAnimPlay(void *prop, const char *anim, uint32_t flags, float weight, std::string *err);   // its length; 0 no file, -1 refused
bool PropAnimStop(void *prop, std::string *err);
int PropAnimState(void *prop, MmlPropAnim *out);
bool PropPhysics(void *prop, const MmlPropPhysics *p, std::string *err);   // p null: off
bool PropPush(void *prop, const float impulse[3], const float *at, std::string *err);
void *PropBody(void *prop);
void PropsFrame();                        // every frame of a loaded mission: physics asked again made
// the game's files and models (models.cpp): the archives' names, a model frame's file, .4ds read and parsed
int GameFiles(const char *pattern, MmlFileFn fn, void *user);
int ModelFile(void *frame, std::string *out);
int ModelOpen(MmlMod *mod, const char *file, std::string *err);
int ModelNodes(int model, MmlModelNode *out, int max);
int ModelMesh(int model, int node, int lod, MmlModelMesh *out);
int ModelMaterials(int model, MmlModelMaterial *out, int max);
void ModelClose(MmlMod *mod, int model);
void ModelsModGone(MmlMod *mod);          // (its unload) its models closed
bool ModelsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// A single mesh's LODs as its .4ds keeps them (skin.cpp): the vertices, the triangles, LS3D's own skin (the vertices
// weighted by no bone, the last `rigid`; a record per bone, in the joints' ids' order, with its joint's name).
struct ModelSkinLod {
    uint32_t vertices = 0, rigid = 0;
    std::vector<float> pos, nrm, uv;
    std::vector<uint16_t> idx;
    struct Bone { std::string joint; float bind[16]; uint32_t one, blended, other; std::vector<float> weights; };
    std::vector<Bone> bones;
};
bool ModelSkin(const char *file, const char *mesh, std::vector<ModelSkinLod> *out, std::string *meshName, std::string *err);
// the meshes the game draws (meshes.cpp): the scene's around the mods' boxes, an actor's
bool SceneFocus(MmlMod *mod, const float *lo, const float *hi);   // null lo: none
bool SceneReady(MmlMod *mod, const float *lo, const float *hi);   // every visual of the box is among the pieces
int ScenePieces(MmlMod *mod, MmlMeshPiece *out, int max);
int ActorMeshes(void *actor, MmlMeshPiece *out, int max);         // -1: no model
void MeshesFrame();                       // every frame of a loaded mission: the walk, the scans
void MeshesMissionClose();
void MeshesModGone(MmlMod *mod);
bool MeshesMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// shaders on the game's own drawing (shaders.cpp; geom.cpp's face group and draw hooks call in)
bool GeomEnsureHooks(std::string *why);   // (geom.cpp) the face groups' drawing hooked
bool GeomEnsureDip(std::string *why);     // (geom.cpp) the device's DrawIndexedPrimitive hooked
bool ShadersWanted();
void ShadersGroupBegin(int kind, void *visual, uint8_t *entry, int where);   // kind 1 object, 2 lightmapped, 3 billboard
void ShadersGroupEnd();
bool ShadersDraw(void *device9, int type, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned n, void *dip, long *hr);
int ShaderAdd(MmlMod *mod, const MmlShaderRule *rule, std::string *err);
bool ShaderRemove(MmlMod *mod, int handle);   // handle 0: every one of the mod's
void ShadersModGone(MmlMod *mod);
void ShadersFrameEnd();                   // (render.cpp) the frame drawn: the survey done
bool ShadersMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- people's skinning (skin.cpp)
void SkinStart();                         // (game_hooks.cpp, the builds known) PoseLod hooked
void SkinMissionClose();                  // the single meshes forgotten
void SkinModGone(MmlMod *mod);            // its skins dropped
int SkinSet(MmlMod *mod, const char *model, const char *mesh, const MmlSkinLod *lods, int count, std::string *err);   // mml.h skin_set
int SkinGet(MmlMod *mod, const char *model, const char *mesh, int lod, MmlSkinMesh *out, std::string *err);           // mml.h skin_get
// people skinned on the GPU (skin.cpp; drawn by shaders.cpp, motion.cpp)
int SkinGpuAsk(MmlMod *mod, bool on);     // mml.h skin_gpu (null mod: the bridge's wish is kept apart: skin.gpu)
void SkinGpuChanged();                    // every LOD skinned and lit again by whoever does it now
bool SkinGpuWanted();                     // asked for, and the device takes it
void SkinFrameBegin();                    // (render.cpp) a frame of the mission's world
struct SkinGpuDraw {                      // what a GPU-skinned draw binds
    void *decl, *rest, *attrs, *palette, *paletteBefore;   // IDirect3D9 objects (the skin's)
    unsigned restStride, attrStride;
    float c[36 * 4];                      // the vertex shader's c120..c155 (mml/skin.hlsli)
};
struct SkinGpuSaved { void *vb[2]; unsigned off[2], stride[2]; void *tex[2]; unsigned long samp[2][5]; void *decl; unsigned long fvf; };
bool SkinGpuFor(void *dev9, const void *visual, int base, SkinGpuDraw *out);   // a draw of a mesh the GPU skins
bool SkinGpuDrawsLod(void *mesh, int lod);   // a single mesh's LOD the GPU skins (its copies into the drawn buffer: none)
void *SkinGpuShader(int which);           // 0 VsSkin, 1 PsSkin, 2 VsSkinDepth, 3 VsSkinMotion (on the device SkinGpuFor used)
void SkinGpuBind(void *dev9, const SkinGpuDraw &d, SkinGpuSaved *saved);
void SkinGpuUnbind(void *dev9, SkinGpuSaved &saved);
long long SkinGpuTimeBegin();             // the draws' own time while the GPU skins (0: not measured)
void SkinGpuTimeEnd(long long t0);
// a rule's vertex data (vdata.cpp: the second stream of normals, tangents, the mods' attributes; shaders.cpp binds it)
struct VdataSaved { unsigned long fvf; void *vb1; unsigned off1, stride1; };
bool VdataBind(void *dev9, const void *visual, int base, unsigned verts, VdataSaved *saved);
void VdataUnbind(void *dev9, VdataSaved &saved);
int VdataSet(MmlMod *mod, const char *visual, int lod, const float *attr, int vertices, std::string *err);   // mml.h shader_vertex_data
void VdataModGone(MmlMod *mod);
void VdataMissionClose();
void VdataFrameEnd();
std::string VdataState();                 // shader.state's vertex_data (json)
int ShaderConstants(MmlMod *mod, int handle, int reg, const float *values, int count, std::string *err);   // mml.h shader_constants
// MikkTSpace's tangents (vdata.cpp): xyz and the bitangent's sign a vertex (strides in floats)
void MikkTangents(uint32_t verts, const float *pos, int posStride, const float *nrm, int nrmStride, const float *uv, int uvStride,
                  const uint32_t *idx, uint32_t faces, float *tan);
bool SkinMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// the shadow map from the sun and the game's lights near a draw (shadows.cpp)
bool ShaderShadows(MmlMod *mod, const MmlShadowSettings *s);   // null s: the mod no longer wants it
bool ShadowsWanted();
void ShadowsTune(float sunDistance, int casters);   // (shader.shadows) sun_distance; casters 1 LS3D's drawing, 0 the meshes, -1 as it is
void ShadowsModGone(MmlMod *mod);
void ShadowsBeforeWorld(void *driver, void *scene, int(__stdcall *render)(void *, void *));   // (render.cpp) the pass, before the world
void ShadowsMissionClose();
void ShadowsConstants(const void *visual, float *c);   // game.hlsli's c76..c116 for a draw of this visual
::IDirect3DBaseTexture9 *ShadowsTexture();  // the map drawn this frame, or null
// the map drawn this frame as gfx.cpp opens it in Direct3D 11: its shared handle, a count of the maps made (a new
// map, a new handle; the handle null when the map is not shared: DXVK's device, ShadowsTexture copied then), its
// matrix and mml.h's sun_shadow_info; false: none this frame
bool ShadowsShared(void **handle, unsigned *made, float viewProj[16], float info[4]);
void ShadowsState(std::string *json);
void *ActorFrame(void *actor);
int FrameWorld(void *frame, float out[16]);
bool PropsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// sound and music (sound.cpp): LS3D's I3D_sound in 2D and 3D, IShow for music
int SoundPlay(MmlMod *mod, const char *file, void *follow, const float *pos, float volume, float pitch, float rangeNear, float rangeFar, uint32_t flags,
              std::string *err);
bool SoundSet(int id, float volume, float pitch, std::string *err);
bool SoundMove(int id, void *follow, const float *pos, std::string *err);
bool SoundStop(int id, std::string *err);
int SoundState(int id, MmlSoundState *out);
int MusicPlay(MmlMod *mod, const char *file, float volume, uint32_t flags, std::string *err);
bool MusicSet(int id, float volume, std::string *err);
bool MusicStop(int id, std::string *err);
int MusicGame(MmlMod *mod, int on);
void SoundFrame();                        // every frame: sounds that follow frames moved, ended ones released, loops
void SoundModGone(MmlMod *mod);           // (its unload) its sounds and music stopped, the city's music given back
void SoundMissionClose();                 // (before the mission closes) the 3D sounds released
bool SoundMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- the game's videos played with FFmpeg (video.cpp): IShow's Open, Close and full-screen loop hooked
void VideoInit();                         // after the game's hooks and the start's
void VideoFrame();                        // every frame: video.play's video
bool VideoMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// control handed to mods: keys and binding slots kept from the game (input.cpp), the player locked, a car's physics
// replaced (control.cpp)
// (the owner of what the loader itself takes, a capture: not a mod, not the bridge)
inline MmlMod *const kLoaderOwner = reinterpret_cast<MmlMod *>(static_cast<uintptr_t>(1));
// a second owner of the loader's for the game's menus: its pages go one an owner, and the mods' settings (panel.cpp)
// put a mod's page over the list of mods as the game puts Audio and Video over Options
inline MmlMod *const kLoaderPage = reinterpret_cast<MmlMod *>(static_cast<uintptr_t>(2));
inline bool LoaderOwned(const MmlMod *m) { return m == kLoaderOwner || m == kLoaderPage; }
bool KeyCapture(MmlMod *mod, int key, bool on);
bool ActionCapture(MmlMod *mod, int slot, bool on);
void CapturesRemove(MmlMod *mod);
int PlayerLock(MmlMod *mod, int on);
int VehicleHook(MmlMod *mod, MmlVehicleFn fn, void *user);
int VehicleBody(void *car, MmlVehicleBody *read, const MmlVehicleBody *write);
int VehicleKinds(MmlMod *mod, unsigned kinds);   // which vehicles the hook is told of (MML_VEHICLES_*)
void ControlModGone(MmlMod *mod);         // (its unload) its vehicle hook dropped, its lock let go
void ControlMissionClose();               // (the mission closes) the lock forgotten
bool ControlMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// interaction (interact.cpp): zones, use-points in the game's own using objects (its Action's choice menu), timers
uint32_t GameTextId(const std::string &utf8);   // (menus.cpp) a text of the loader's own in the game's database; 0: none
int HudHelpLine(const std::string &utf8);       // the game's help line shown (MissionHelpAdd): the ms it stays, 0 not
int ZoneAdd(MmlMod *mod, const float *pos, float radius, void *frame, MmlZoneFn fn, void *user, std::string *err);
int UseAdd(MmlMod *mod, const float *pos, float radius, void *frame, const char *label, MmlUseFn fn, void *user, std::string *err);
int TimerAdd(MmlMod *mod, int ms, bool repeat, MmlTimerFn fn, void *user, std::string *err);
bool InteractRemove(MmlMod *mod, int id);
bool ZoneWatch(MmlMod *mod, int zone, uint32_t who, void *actor, std::string *err);   // null mod: the bridge's zones
int WorldWater(const float at[3], MmlWater *out);       // (collide.cpp) the water under a point
void InteractFrame();                     // every frame of a loaded mission: crossings, prompts, presses
void InteractTick(int ms);                // the mission's tick: the timers
void InteractModGone(MmlMod *mod);
void InteractMissionClose();
void InteractActorGone(void *actor);      // (an actor's destructor) what follows its model goes
bool InteractMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// the HUD (hud.cpp): pictures by the game's texture loader, the game's console and help lines, markers on the screen,
// the radar and the compass
std::string GameAnsi(const std::string &utf8);  // (menus.cpp) UTF-8 to the game's own encoding
std::string GameUtf8(const std::string &ansi);  // (menus.cpp) ... and back
int HudPictureLoad(MmlMod *mod, const char *file, const char *alpha, std::string *err);   // null mod: the bridge's (kept)
bool HudPictureSize(int picture, int *w, int *h);
void HudPictureFree(MmlMod *mod, int picture);
void *HudPictureTexture(int picture);     // (draw.cpp, inside its guard) the ITexture*, null when freed
int HudConsole(const std::string &utf8, uint32_t argb, std::string *err);
int HudMarkerAdd(MmlMod *mod, const MmlHudMarker *m, std::string *err);
bool HudMarkerSet(MmlMod *mod, int id, const MmlHudMarker *m, std::string *err);
bool HudMarkerRemove(MmlMod *mod, int id);
void HudPass();                           // (draw.cpp, each 2D pass) the markers queued where the camera puts them
int HudGame(MmlMod *mod, int show, std::string *err);
bool HudGameHidden();                     // (draw.cpp's hook of DrawAll) the game's HUD not to be drawn now   // the game's HUD hidden (0), shown (1), asked (-1): 1 shown, 0 hidden, -1 not known
void HudModGone(MmlMod *mod);
void HudMissionClose();                   // (before the game's) markers, their frames, the compass
void HudActorGone(void *actor);
bool HudMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// a mod's state in the game's saves (saves.cpp): beside each save of the game, and the profile's
void SavesInit();                         // (after the game's hooks) SaveGameSave, SaveGameLoad, SaveGameNewGame, ProfileDelete
int SavePut(MmlMod *mod, int scope, const char *key, const void *data, size_t n, std::string *err);   // null mod: the bridge's
size_t SaveGet(MmlMod *mod, int scope, const char *key, void *data, size_t cap);
bool SavesMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
int SavePutAs(const std::string &owner, int scope, const char *key, const void *data, size_t n, std::string *err);   // the host's own
size_t SaveGetAs(const std::string &owner, int scope, const char *key, void *data, size_t cap);
int SaveProfileNumber();                  // the profile selected (savegame\mafia<number>.sav), -1: none
// the statistics and the profile's progress (stats.cpp)
void StatsEvent(const MmlEvent &ev);      // (mods.cpp Dispatch) every event: what the player did
void StatsFrame(float dt, bool ticking);  // (game_hooks.cpp) every frame of a loaded mission: time, distances
int StatsGet(int scope, MmlStats *out);
int ProfileProgress(MmlProgress *out);
int ProfileSaves(int *slots, int max);
bool StatsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
std::string WorldCarName(int index);      // (world.cpp) the car database's name of a car, "" none
// a mod's own items (items.cpp): records added to the game's items, their ids kept in mml/items.json
const char *GameTextOwn(uint32_t id);                       // (menus.cpp) the game's own text of an id, or null
bool GameTextSet(uint32_t id, const std::string &utf8);     // (menus.cpp) one of the game's ids answered by the loader
bool GameTextsReady();                                      // (menus.cpp) the text database hooked (once)
void ItemsFrame();                        // every frame: the table ours once the game has it (ids kept from earlier runs)
int ItemDefine(const std::string &mod, const MmlItemDef &def, std::string *err);   // the id, 0: refused
int ItemFind(const std::string &mod, const std::string &name);
std::string ItemOwner(int id);            // "mod/name" of a mod's item, "" for the game's
bool ItemsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// people's animations, getting up, the traffic pedestrians' hits (anims.cpp)
bool HumanBones(void *human, void **frames);   // (humans.cpp) the skeleton's frames of a person the host keeps
bool HumanKnown(void *human);
bool HumanPosable(void *human);           // a person (humans_list) or a traffic pedestrian's element: a skeleton to pose
bool HumanLastPose(void *human, float world[][16], unsigned maxAgeMs);   // the bones' world matrices last drawn by a mod or the bridge
// what strikes people, before the game (hits.cpp): the mods' hit filters, the game's Hit called, the pedestrians'
// bullet Hit hooked; hits.state, human.hit
void HitsInit();                          // (after AnimsInit) the pedestrian system's bullet Hit
bool HitsFilter(void *person, int pedestrian, MmlHumanHit *hit);   // true: a mod took it (the game's Hit must not run)
bool HitsInPedBullet(void *element, MmlVec3 *pos = nullptr);   // pedBulletHit filtered this element and runs (pedHit's filter
                                                               // passes it by); pos: the point the bullet struck
int HitFilterSet(MmlMod *mod, MmlHitFilterFn fn, void *user);
int HumanHitCall(void *person, const MmlHumanHit *hit, std::string *err);
void HitsModGone(MmlMod *mod);
void HitsActorGone(void *actor);
void HitsMissionClose();
int HitsShapeOf(void *person);            // 1 + the capsule the last bullet stretch gave him (0 none)
int BulletShapeSet(MmlMod *mod, void *person, const MmlCapsule *parts, int count, std::string *err);
bool HitsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// the traffic's pedestrians (pedestrians.cpp): their elements, from the managers' arrays and the game's list
std::vector<void *> PedestrianElements();
bool PedestrianKnown(void *element);
void *PedestrianOfFrame(void *model);     // the element whose model frame it is, or null
bool PedestrianRead(void *element, MmlPedestrian *out);
float PedestrianHealth(void *element);
bool PedestriansMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// a frame's release told (framelife.cpp): LS3D's frames' and meshes' Release hooked once something is watched
void FrameLifeInit();
void FixesInit();                         // (fixes.cpp, after the game's hooks) bugs of the game itself, always fixed
// the size things are drawn at (scale.cpp): an actor's or a pedestrian's model frame scaled for the world's drawing
// alone (render.cpp around the world's Render); actor.scale, actors.scaled
int ActorScale(MmlMod *mod, void *actor, const MmlActorScale *s, std::string *err);   // s null: its own size
void ScaleBeat();                         // the pulses' shared beat starts now
void ScaleBeforeRender();
void ScaleAfterRender();
void ScaleActorGone(void *actor);
void CameraFollowGone(void *actor);    // (view.cpp) camera.set's followed actor destroyed: the camera given back
void ScaleModGone(MmlMod *mod);
void ScaleMissionClose();
bool ScaleMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
// drawing in the world (worlddraw.cpp): lines, boxes, spheres, capsules over the 3D world at its stage 1
int DrawWorldAdd(MmlMod *mod, const MmlDrawShape *shape, std::string *err);   // shape null: the mod's taken away
bool DrawWorldWanted();
void DrawWorldNow(void *device9);         // (render.cpp: the game's IDirect3DDevice9, its state kept around it)
void DrawWorldModGone(MmlMod *mod);
bool DrawWorldMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
int FrameWatch(MmlMod *mod, void *frame, MmlFrameGoneFn fn, void *user, bool bridge, std::string *err);
void FrameLifeModGone(MmlMod *mod);
bool FrameLifeMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
void AnimsInit();                         // (after the game's hooks) the pedestrians' Hit
int HumanAnimPlay(void *human, const char *anim, int flags, std::string *err);   // its length (ms), -1 refused
int HumanAnimStop(void *human, std::string *err);
int HumanAnimReplace(MmlMod *mod, void *human, const char *own, const char *with, std::string *err);
int HumanAnimState(void *human, MmlHumanAnim *out);
int HumanGetUp(void *human, int blendMs, std::string *err);   // 1 from the back, 2 from the front, 0 refused
void AnimsBeforeRender();                 // (humans.cpp, RENDER_BEGIN) the get-ups' blends
void AnimsModGone(MmlMod *mod);
void AnimsActorGone(void *actor);
void AnimsMissionClose();
bool AnimsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
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
int GfxStateNow(std::string *why);        // mml.h gfx_state: 1 on, 0 off (why), -1 not yet
void GfxShowStats(MmlGfxShowStats *out);  // mml.h gfx_show_stats (out's size checked by the caller)
void GfxRemove(MmlMod *mod);              // its mod unloads: its passes go
void GfxQuiet(bool on);                   // drawing that is not the game's (not tallied as the world's light)
void GfxDrawnSize(float out[4]);          // the size this frame's world is drawn at: w, h, 1/w, 1/h (gfx_upscale's, else the frame's)
void GfxFrameBegin();                     // (render.cpp) a frame's first drawing: its GPU time starts
int GfxUpscaleSet(MmlMod *mod, unsigned w, unsigned h, MmlGfxUpscaleFn fn, void *user);   // mml.h gfx_upscale
// ---- motion vectors (motion.cpp; mml.h gfx_motion, MmlGfxFrame motion)
bool MotionWanted();                      // a mod wants them
int MotionAsk(MmlMod *mod, bool on);      // mml.h gfx_motion
void MotionRemove(MmlMod *mod);           // its mod unloads
// gfx.cpp, the frame's world about to be drawn: the objects' target (IDirect3DSurface9*, cleared), the last frame's
// camera (null: none); MotionFrameEnd at its stage 1; MotionDeviceGone when the device goes
void MotionFrameBegin(void *dev9, void *target, const float *viewBefore, const float *projBefore);
void MotionFrameEnd();
void MotionDeviceGone();
// geom.cpp: the face group LS3D draws (kind 1 object, 2 lightmapped, 3 billboard) and its draws, after the game's
void MotionGroupBegin(int kind, const void *visual, const void *group, bool inFrame);
void MotionGroupEnd();
void MotionAfterDraw(void *dev9, int type, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned n, void *dip);
std::string MotionState();                // gfx.state's motion (json)
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
// the scene drawn by LS3D from an orthographic camera at `world` (rows right, up, forward, eye), `half` metres each
// way, depth nearZ..farZ, into the targets the caller has bound (size x size): its view x projection and the backdrop's
// view out (shadows.cpp's map); ShadowViewNow: that drawing is under way
bool ViewDrawShadow(void *driver, void *scene, RenderFn render, const float world[16], float half, float nearZ, float farZ, int size,
                    float viewProj[16], float backView[16], std::string *why);
bool ShadowViewNow();
void ShadersShadowBegin(const float backView[16]);   // (shaders.cpp) the drawing from the sun begins
void ShadersShadowStats(unsigned *draws, unsigned *faces, unsigned *skipped, unsigned *cut, float viewProj[16], std::string *why);
// the frame's camera overridden (view.cpp): camera_override; camera.state, camera.set, camera.release
int CameraOverride(MmlMod *mod, MmlViewFn fn, void *user);
void FrameCameraBegin(void *scene);       // (render.cpp) before the views and the frame's own drawing
void FrameCameraEnd();                    // ... after the frame's drawing: the game's camera back
int RenderProject(const float world[3], float screen[3]);   // mml.h camera_project (render.cpp)
bool RenderFrameCamera(MmlRenderCamera *out);              // the camera the mission's world was last drawn with
int RenderRay(float x, float y, float origin[3], float dir[3]);   // mml.h camera_ray
bool CameraMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);

// ---- what happens (happen.cpp): shots, explosions, cars blown up, deaths, vehicles entered and left
void HappenInit();                        // (game_hooks.cpp) once the build is known
void HappenHit(void *human, void *attacker);
void HappenFrame();                       // every frame of a loaded mission
void HappenMissionClosed();
// A save loaded in the mission (a restore point, the Game Over's retry, an arrest: the mission not closed) or a new
// game: what the frames compare (the money, each person's items and state) taken anew, not told as changes.
void HappenGameLoaded();

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
// A frame is from one return of the game's own Present to the next (screenshot.cpp's Present hook calls these around
// it): its time, the time Present blocked, the game thread's and the process's CPU time, the GPU's (d3dstats.cpp), the
// device's calls, the host's own costly sites; kept a frame each in a ring (prof.frames), labelled for comparisons
// (prof.mark).
void ProfPresentBegin();
void ProfPresentEnd(long hr);
uint32_t ProfFrameSeq();                  // the frame being made now
void ProfGpuOf(uint32_t seq, float ms);   // (d3dstats.cpp) the GPU's time of a frame, known a few frames later
double ProfTscPerMs();                    // the time stamp counter's rate (0 until known)
bool ProfFrameStats(const MmlMod *mod, MmlFrameStats *out);   // mml.h frame_stats
// The host's own costly sites, timed while the device's calls are counted (prof.counters): a hook's whole time and the
// game's own function it calls (its share), so the host's part is the difference.
// The visuals' sites (item 31a): each class's m_AddFGroups whole (LS3D's work for a visual drawn: its LOD, its
// lighting, its vertices' copy, its face groups queued) and the LightVertices under it (skin.cpp's hook, told the
// class by g_profVisual).
enum ProfHot {
    kHotSkinLod, kHotSkinLodGame, kHotSkinCopy, kHotSkinCopyGame, kHotSkinLight, kHotSkinLightGame,
    kHotVisual, kHotVisualEnd = kHotVisual + 5, kHotLighting = kHotVisualEnd, kHotLightingEnd = kHotLighting + 5,
    kHotCount = kHotLightingEnd
};
extern bool g_profHotOn;
extern int g_profVisual;   // the class whose m_AddFGroups runs now (0..4), -1 none (game thread)
extern uint64_t g_profHotTicks[kHotCount];
extern uint32_t g_profHotCalls[kHotCount];
struct ProfHotScope {
    int site;
    uint64_t t0;
    explicit ProfHotScope(int s) : site(s), t0(g_profHotOn ? __rdtsc() : 0) {}
    ~ProfHotScope() { if (t0) { g_profHotTicks[site] += __rdtsc() - t0; g_profHotCalls[site]++; } }
    ProfHotScope(const ProfHotScope &) = delete;
    ProfHotScope &operator=(const ProfHotScope &) = delete;
};

// ---- the Direct3D 9 device's calls counted and timed a frame, by kind and by caller (the game, the host), the time
// in the driver under the draws, the pipeline's switches, the GPU's time of each frame (d3dstats.cpp); prof.counters
enum D3dWho { kWhoGame, kWhoHost, kWhoCount };
enum D3dKind {
    kKindDrawPrimitive, kKindDrawIndexed, kKindDrawUp, kKindRenderState, kKindTextureStage, kKindSampler, kKindTexture,
    kKindTransform, kKindLight, kKindMaterial, kKindVertexFormat, kKindStream, kKindIndices, kKindShader,
    kKindShaderConstants, kKindViewport, kKindTargets, kKindClear, kKindScene, kKindStateBlock, kKindGet, kKindLock,
    kKindCount
};
struct D3dFrameCounts {
    uint32_t calls[kKindCount][kWhoCount];
    uint64_t ticks[kKindCount][kWhoCount];   // inclusive: a game's draw holds the host's work in its chain
    uint32_t prims[kWhoCount];
    uint64_t driverTicks[kWhoCount];         // under the draws: the time past every hook (whose draw it was)
    uint32_t driverDraws[2];                 // the draws reaching the driver: [0] fixed pipeline, [1] shaders
    uint32_t switches;                       // between the fixed pipeline and shaders, draw to draw at the driver
};
const char *D3dKindName(int kind);
bool D3dStatsOn();
bool D3dStatsSet(bool on, bool gpu, std::string *why);   // game thread; gpu: each frame's GPU time too
void D3dStatsTake(D3dFrameCounts *out);        // this frame's, zeroed after (prof.cpp at a frame's end)
void D3dStatsFrameEnd(uint32_t seq);           // before the game's Present: the GPU's mark of the frame's end
void D3dStatsFrameStart();                     // after it: the next frame's start
std::string D3dStatsWhyGpu();                  // "" while the GPU's time is measured
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
