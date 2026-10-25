// The start: the game taken from its launch to a chosen profile and freeride by its own menus, and the menus driven
// by agents (bridge methods menu.state, menu.command, start.state).
//
// A start is asked for once, by <game>\mml\start.json (the MCP server's game_launch writes it; it is deleted when
// read, so the next launch is the game's usual one):
//   {"skip_logos": true, "profile": 4 | "name", "go": "menu" | "freeride", "traffic": 1, "pedestrians": 1, "police": 0,
//    "background": true, "resolution": [1920, 1080]}
// - resolution: the screen at this size for this run, whatever Setup.exe chose (its setting is the registry's, one
//   for every game on the machine, and is left as it is: AskResolution);
// - skip_logos: the three logo videos InitSystem plays before the profile menu are not played (PlayVideo answers
//   "played" for logo1, logo2, logo3 only: the videos missions play are left alone). 1.0 plays two AVI logos,
//   logo1.avi and logo2.avi, and only when cd_find_game_file finds them (on the game's disc): it is answered "not
//   found" for those two;
// - profile: in the profile menu, the profile with this number (savegame\mafiaNNN.sav) or name is taken, by the
//   command a click on it sends (100 + its place in the list the game has read);
// - go: "freeride" sends the main menu's freeride command, then the freeride menu's start: the game's default
//   freeride with the menu's own default settings (missions\freeitaly). "menu" stops in the main menu; "load" opens
//   the load menu and takes a save; "new" starts a new game (the intro mission, then mise01); "extreme" the
//   freeride extreme (missions\extreme);
// - kind ("freeride"): city-small (the default, freeitaly), city-night (freeridenoc), countryside-night
//   (freekrajinanoc), city (freeride), countryside (freekrajina): the kind menu's row (command 100 of the freeride
//   menu opens it); for a kind other than city-small the start opens the spawn menu, where place (1..: its row, or a
//   part of its text: "Down Town") is taken (default 1). A kind the profile has not opened (the menu hides it) is
//   started all the same, and start.state says so;
// - save ("load"): the save's number (savegame\mafiaNNN.<number>, the profile's) or a part of its name ("Old
//   Prison"); several names matched or none: the start fails and lists them;
// - traffic, pedestrians, police (0..1; each optional): the freeride menu's three density sliders, set before its
//   start by the setter the menu itself sets them with ("police": 0 is a city without patrols). The game uses
//   them only for a profile that has opened the freeride's settings; the log says when it has not.
// - background: the game never makes its own window the active one (the user keeps working in the window in front):
//   user32's ShowWindow shows the game's windows without activating them, SetForegroundWindow, SetActiveWindow and BringWindowToTop are answered
//   as done for them, SetWindowPos gets SWP_NOACTIVATE; while the game's window is not the one in front, its
//   top-level windows are shown at the bottom of the windows' order (CreateWindowEx with WS_VISIBLE, ShowWindow:
//   a window shown goes on top even unactivated) and SetWindowPos keeps their place there (SWP_NOZORDER), and its
//   SetFocus, ClipCursor and SetCursorPos do nothing (LS3D would take the focus, keep the user's cursor in its
//   window, put it in its middle). A click on the game's window activates it as usual (that does not go through
//   these calls), and in front the game has the mouse as before. The game runs and draws behind other windows;
//   screenshots need a renderer whose frame can be read there (Direct3D 9, dxwrapper's), not plain Direct3D 8 with
//   antialiasing (screenshot.cpp copies the screen then).
// The bridge's menu.value {id, value} reads (and sets) such a component of the active menu.
// Each step waits for its menu to be the active one and sends one command, as a click would, from the frame
// limiter's call inside the menu's loop (the loop the click handlers run in). The menus' addresses: game_build.h.
// In 1.0 a command is the screen's next tick's answer (menus10.cpp): the screen does with it what a click does, a
// frame later; its ids are 1.2's (the profiles 100 + i, the main menu's 990105, the freeride's 201).
#include "host.h"
#include <fstream>
#include "json.hpp"
#include "menus_shared.h"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

// The freeride's kinds, as its kind menu ("freeridetrack") offers them: the command a click sends, the result it
// closes with (what MafiaEntryPoint opens: 28 freeitaly ... 32 freekrajinanoc), the mission, and the level of the
// profile's freeride progress (loadSave + 0x1F0) from which the menu shows the row (0: always; 1: 402, 403; 2: all).
struct Kind { const char *name, *alias, *mission; int command, result, unlock; };
const Kind kKinds[] = {
    {"city-small", "small", "freeitaly", 401, 28, 0},
    {"city-night", "night", "freeridenoc", 402, 30, 1},
    {"countryside-night", "", "freekrajinanoc", 403, 32, 1},
    {"city", "city-day", "freeride", 404, 29, 2},
    {"countryside", "countryside-day", "freekrajina", 405, 31, 2},
};

struct Request {
    bool active = false, skipLogos = false;
    int profileNumber = -1;        // -1: by name, or none
    std::string profileName;       // empty: by number, or none
    std::string go;                // "", "menu", "freeride", "load", "new", "extreme"
    float density[3] = {-1, -1, -1};   // the freeride's traffic, pedestrians, police (below 0: the menu's own)
    const Kind *kind = nullptr;    // the freeride's kind (null: the menu's default, city-small)
    int placeNumber = 0;           // the spawn menu's row: 1.. (0: by name, or the first)
    std::string placeName;
    int saveNumber = -1;           // the save to load: its number (savegame\mafiaNNN.<number>), or by name
    std::string saveName;
    bool profileDone = false, mainDone = false, freerideDone = false, kindAsked = false, kindChosen = false, done = false;
    std::string state = "none", error, note;
    DWORD started = 0;
};
Request g_req;
bool g_menuOk;                     // the build's menu addresses are known

using PlayVideoFn = bool(__fastcall *)(const char *name, void *edx, float w, float h);   // thiscall, ret 8
PlayVideoFn g_playVideo;

bool __fastcall PlayVideoHook(const char *name, void *edx, float w, float h) {
    if (g_req.skipLogos && name && (!_stricmp(name, "logo1") || !_stricmp(name, "logo2") || !_stricmp(name, "logo3"))) {
        HLOG("start: logo video '%s' skipped", name);
        return true;
    }
    return g_playVideo(name, edx, w, h);
}

// 1.0: cd_find_game_file, fastcall(name, char *path) -> bool; the logos are played only when it finds them.
using CdFindFn = bool(__fastcall *)(const char *name, char *path);
CdFindFn g_cdFind;
bool __fastcall CdFindHook(const char *name, char *path) {
    if (g_req.skipLogos && name && (!_stricmp(name, "logo1.avi") || !_stricmp(name, "logo2.avi"))) {
        HLOG("start: logo video '%s' skipped", name);
        return false;
    }
    return g_cdFind(name, path);
}

uint32_t Rd(uintptr_t a) {
    uint32_t v = 0;
    MemRead(a, &v, 4);
    return v;
}

// ---- the game kept in the background
bool Own(HWND h) {
    DWORD pid = 0;
    return h && GetWindowThreadProcessId(h, &pid) && pid == GetCurrentProcessId();
}
using ShowWindowFn = BOOL(WINAPI *)(HWND, int);
using HwndFn = BOOL(WINAPI *)(HWND);
using ActiveFn = HWND(WINAPI *)(HWND);
using SetPosFn = BOOL(WINAPI *)(HWND, HWND, int, int, int, int, UINT);
using ClipFn = BOOL(WINAPI *)(const RECT *);
using CursorPosFn = BOOL(WINAPI *)(int, int);
ActiveFn oSetFocus;
ClipFn oClipCursor;
CursorPosFn oSetCursorPos;
bool GameInFront() { return Own(GetForegroundWindow()); }
HWND WINAPI hkSetFocus(HWND h) { return Own(h) && !GameInFront() ? GetFocus() : oSetFocus(h); }
BOOL WINAPI hkClipCursor(const RECT *r) { return r && !GameInFront() ? TRUE : oClipCursor(r); }
BOOL WINAPI hkSetCursorPos(int x, int y) { return GameInFront() ? oSetCursorPos(x, y) : TRUE; }
ShowWindowFn oShowWindow;
HwndFn oSetForeground, oBringToTop;
ActiveFn oSetActive;
SetPosFn oSetWindowPos;
// A window shown is put on top of the others even when it is not activated (measured: LS3D's window came up over
// the user's in front, without the focus): while the game is not in front, its top-level windows are shown at the
// bottom of the order and keep their place there; a click on one brings it up as usual.
bool TopLevel(HWND h) { return Own(h) && !(GetWindowLongW(h, GWL_STYLE) & WS_CHILD); }
void ShowAtBottom(HWND h) {
    (oSetWindowPos ? oSetWindowPos : SetWindowPos)(h, HWND_BOTTOM, 0, 0, 0, 0,
                                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}
BOOL WINAPI hkShowWindow(HWND h, int cmd) {
    if (Own(h)) {
        if (cmd == SW_SHOWNORMAL || cmd == SW_RESTORE || cmd == SW_SHOWDEFAULT) cmd = SW_SHOWNOACTIVATE;
        else if (cmd == SW_SHOW || cmd == SW_SHOWMAXIMIZED) cmd = SW_SHOWNA;
        if ((cmd == SW_SHOWNOACTIVATE || cmd == SW_SHOWNA) && TopLevel(h) && !IsWindowVisible(h) && !IsIconic(h) && !GameInFront()) {
            ShowAtBottom(h);   // (shown where it is, at its size)
            return FALSE;      // (it was hidden)
        }
    }
    return oShowWindow(h, cmd);
}
BOOL WINAPI hkSetForeground(HWND h) { return Own(h) ? TRUE : oSetForeground(h); }
BOOL WINAPI hkBringToTop(HWND h) { return Own(h) ? TRUE : oBringToTop(h); }
HWND WINAPI hkSetActive(HWND h) { return Own(h) ? GetActiveWindow() : oSetActive(h); }
BOOL WINAPI hkSetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (Own(h)) {
        flags |= SWP_NOACTIVATE;
        if (TopLevel(h) && after != HWND_BOTTOM && !GameInFront()) flags |= SWP_NOZORDER;   // (not raised, nor made topmost)
    }
    return oSetWindowPos(h, after, x, y, cx, cy, flags);
}
// LS3D makes the game's window visible at once (WS_VISIBLE), so it goes on top as it is made: made hidden, then shown
// at the bottom. Taken in LS3DF.dll's imports (user32's own CreateWindowExA is not hooked: a wrapper's hook already
// sits on its first bytes, MinHook refuses it).
using CreateAFn = HWND(WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateAFn oCreateA;
HWND WINAPI hkCreateA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
                      HINSTANCE inst, LPVOID param) {
    bool hide = (style & WS_VISIBLE) && !(style & WS_CHILD) && !GameInFront();
    HWND r = oCreateA(ex, cls, name, hide ? style & ~WS_VISIBLE : style, x, y, w, h, parent, menu, inst, param);
    if (r && hide) ShowAtBottom(r);
    return r;
}
void KeepInBackground() {
    HMODULE u = GetModuleHandleA("user32.dll");   // (the game imports it; no LoadLibrary under the loader lock)
    struct { const char *name; void *detour; void **orig; } hooks[] = {
        {"ShowWindow", reinterpret_cast<void *>(&hkShowWindow), reinterpret_cast<void **>(&oShowWindow)},
        {"SetForegroundWindow", reinterpret_cast<void *>(&hkSetForeground), reinterpret_cast<void **>(&oSetForeground)},
        {"BringWindowToTop", reinterpret_cast<void *>(&hkBringToTop), reinterpret_cast<void **>(&oBringToTop)},
        {"SetActiveWindow", reinterpret_cast<void *>(&hkSetActive), reinterpret_cast<void **>(&oSetActive)},
        {"SetWindowPos", reinterpret_cast<void *>(&hkSetWindowPos), reinterpret_cast<void **>(&oSetWindowPos)},
        {"SetFocus", reinterpret_cast<void *>(&hkSetFocus), reinterpret_cast<void **>(&oSetFocus)},
        {"ClipCursor", reinterpret_cast<void *>(&hkClipCursor), reinterpret_cast<void **>(&oClipCursor)},
        {"SetCursorPos", reinterpret_cast<void *>(&hkSetCursorPos), reinterpret_cast<void **>(&oSetCursorPos)},
    };
    for (auto &h : hooks) {
        std::string err;
        uintptr_t at = u ? reinterpret_cast<uintptr_t>(GetProcAddress(u, h.name)) : 0;
        if (!at || !HookAdd(nullptr, at, h.detour, h.orig, &err)) HWARN("start: background: cannot hook %s: %s", h.name, err.c_str());
    }
    HMODULE ls3d = GetModuleHandleA("LS3DF.dll");
    void *old = nullptr;
    if (ls3d && PatchImport(ls3d, "user32.dll", "CreateWindowExA", reinterpret_cast<void *>(&hkCreateA), &old))
        oCreateA = reinterpret_cast<CreateAFn>(old);
    else
        HWARN("start: background: LS3DF.dll's CreateWindowExA not taken: its window will come up over the others");
    HLOG("start: the game's window is kept in the background (the game does not activate it)");
}

// The screen's size asked for (start.json "resolution": [w, h]). The game's display settings are the registry's
// LS3D_setup (HKCU\Software\Illusion Softworks\Mafia: Setup.exe writes it, one value for every game on the machine),
// read once by MafiaEntryPoint before InitSystem: RegQueryValueExA into InitSettings, 63 bytes, byte 0 the version
// (0x14, or the game takes its defaults), the width a u32 at +2, the height at +6 (both builds: 1.2's InitSettings
// 0x647EE4, 1.0's 0x6F952C, the globals InitSystem reads the size from). Answered with this size in this process
// only: the registry is not written, so the player's own game and the other lab keep theirs. The same for Setup's
// Fullscreen (+0x15: InitSystem makes it IGraph::Init's flag 2, a device with Windowed false; 1.2 0x647EF9, 1.0
// 0x6F9541) and the adapter (+1: IGraph::Init's adapter, Direct3D's ordinal).
// The game writes LS3D_setup back when it quits by its menu (MafiaEntryPoint: InitSettings with the options' details,
// draw distance, effects, sounds, shadows and particles of the run): the values answered for this run are given their
// registry's own again in that write (RegSetValueExA), so the options are kept and the screen stays Setup.exe's.
// (1.0 writes where it reads, HKEY_LOCAL_MACHINE; 1.2 wrote HKEY_LOCAL_MACHINE too, which it does not read: fixes.cpp
// points its write where it reads, HKEY_CURRENT_USER.)
unsigned g_resW, g_resH;
int g_fullAsked = -1, g_adapterAsked = -1;   // (-1: Setup's own)
BYTE g_setupRead[0x16];   // the registry's own first bytes of LS3D_setup, as read (the size, the full screen, the adapter)
bool g_setupKept;
using RegQueryValueExAFn = LSTATUS(WINAPI *)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
RegQueryValueExAFn oRegQueryValueExA;
using RegSetValueExAFn = LSTATUS(WINAPI *)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
RegSetValueExAFn oRegSetValueExA;
LSTATUS WINAPI hkRegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size) {
    if (g_setupKept && name && !_stricmp(name, "LS3D_setup") && data && size >= sizeof g_setupRead && size <= 0x100 && data[0] == 0x14) {
        BYTE out[0x100];
        memcpy(out, data, size);
        if (g_resW) memcpy(out + 2, g_setupRead + 2, 8);
        if (g_fullAsked >= 0) out[0x15] = g_setupRead[0x15];
        if (g_adapterAsked >= 0) out[1] = g_setupRead[1];
        HLOG("start: LS3D_setup written by the game with the registry's own screen (the options of the run kept)");
        return oRegSetValueExA(key, name, reserved, type, out, size);
    }
    return oRegSetValueExA(key, name, reserved, type, data, size);
}
LSTATUS WINAPI hkRegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
    LSTATUS r = oRegQueryValueExA(key, name, reserved, type, data, size);
    if (r == ERROR_SUCCESS && name && !_stricmp(name, "LS3D_setup") && data && size && *size >= 0x16 && data[0] == 0x14) {
        memcpy(g_setupRead, data, sizeof g_setupRead);
        g_setupKept = oRegSetValueExA != nullptr;
        if (g_resW) {
            unsigned was[2];
            memcpy(was, data + 2, sizeof was);
            memcpy(data + 2, &g_resW, 4);
            memcpy(data + 6, &g_resH, 4);
            HLOG("start: the screen at %ux%u (Setup.exe's %ux%u in the registry, left as it is)", g_resW, g_resH, was[0], was[1]);
        }
        if (g_fullAsked >= 0) {
            HLOG("start: %s (Setup.exe's %s in the registry, left as it is)", g_fullAsked ? "full screen" : "in a window",
                 data[0x15] ? "full screen" : "window");
            data[0x15] = static_cast<BYTE>(g_fullAsked);
        }
        if (g_adapterAsked >= 0) {
            HLOG("start: the adapter %d (Setup.exe's %u in the registry, left as it is)", g_adapterAsked, data[1]);
            data[1] = static_cast<BYTE>(g_adapterAsked);
        }
    }
    return r;
}
bool TakeSetupRead() {
    if (oRegQueryValueExA) return true;
    void *old = nullptr;
    if (!PatchImport(GetModuleHandleW(nullptr), "advapi32.dll", "RegQueryValueExA", reinterpret_cast<void *>(&hkRegQueryValueExA), &old)) {
        HWARN("start: Game.exe's RegQueryValueExA not taken: the screen stays as Setup.exe set it");
        return false;
    }
    oRegQueryValueExA = reinterpret_cast<RegQueryValueExAFn>(old);
    if (PatchImport(GetModuleHandleW(nullptr), "advapi32.dll", "RegSetValueExA", reinterpret_cast<void *>(&hkRegSetValueExA), &old))
        oRegSetValueExA = reinterpret_cast<RegSetValueExAFn>(old);
    else   // (then the run's screen goes into the registry when the game quits by its menu)
        HWARN("start: Game.exe's RegSetValueExA not taken: the screen asked for this run is written when the game quits");
    return true;
}
void AskResolution(unsigned w, unsigned h) {
    if (!TakeSetupRead()) return;
    g_resW = w;
    g_resH = h;
}

uintptr_t TopMenu() { return g_menuOk ? Rd(game::Exe().menuTop) : 0; }
// The menu it was opened over (1.2: +4; 1.0's screens: +0x18).
uintptr_t UnderMenu(uintptr_t m) { return m10::Known() ? m10::Under(m) : Rd(m + 4); }

const char *MenuName(uintptr_t vt) {
    const game::ExeBuild &e = game::Exe();
    if (vt == e.menuProfileVt) return "profile";
    if (vt == e.menuMainVt) return "main";
    if (vt == e.menuFreerideVt) return "freeride";
    return "";
}

// A menu's command, as a click sends it: its vtable's +0x24, thiscall(menu, command). Game thread only. 1.0: the
// screen's next tick gets it (menus10.cpp).
using CommandFn = int(__fastcall *)(uintptr_t menu, void *edx, int command);
bool SafeCommand(uintptr_t menu, int command) {
    if (m10::Known()) return m10::Command(menu, command);
    __try {
        auto fn = reinterpret_cast<CommandFn>(Rd(Rd(menu) + 0x24));
        fn(menu, nullptr, command);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A component of a menu by its id: its value read, or set, through the menu's own functions (game_build.h). Game
// thread only. False: the build's functions are not known, or the call faulted.
bool SafeGetValue(uintptr_t menu, int id, float *value) {
    using GetFn = float(__fastcall *)(uintptr_t menu, void *edx, int id);
    if (!game::Exe().menuGetValue) return false;
    __try {
        *value = reinterpret_cast<GetFn>(game::Exe().menuGetValue)(menu, nullptr, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeSetValue(uintptr_t menu, int id, float value) {
    using SetFn = int(__fastcall *)(uintptr_t menu, void *edx, int id, float value);
    if (!game::Exe().menuSetValue) return false;
    __try {
        reinterpret_cast<SetFn>(game::Exe().menuSetValue)(menu, nullptr, id, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Profile { int number; std::string name; };
std::vector<Profile> Profiles() {
    std::vector<Profile> out;
    if (!g_menuOk) return out;
    uintptr_t ls = game::Exe().loadSave;
    uint32_t b = Rd(ls + 4), e = Rd(ls + 8);
    if (!b || e < b || (e - b) % 0x54 || (e - b) / 0x54 > 1000) return out;
    for (uint32_t r = b; r < e; r += 0x54) {
        char name[0x45] = {};
        MemRead(r + 0x10, name, 0x44);
        out.push_back({static_cast<int>(Rd(r + 8)), name});
    }
    return out;
}

void Fail(const std::string &why) {
    g_req.error = why;
    g_req.state = "failed";
    g_req.active = false;
    HWARN("start: %s; the menus are left to the player", why.c_str());
}

void Done(const char *what) {
    g_req.state = "done";
    g_req.done = true;
    g_req.active = false;
    HLOG("start: %s (%.1f s after the launch)", what, (GetTickCount() - g_req.started) / 1000.0);
}

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// A menu's rows (their id and text, UTF-8), from the description menu.state shows.
std::vector<std::pair<int, std::string>> Rows(uintptr_t menu) {
    std::vector<std::pair<int, std::string>> out;
    try {
        json d = json::parse(MenuDescribe(menu));
        for (auto &c : d["controls"])
            if (c.value("kind", "") == "item" || c.value("kind", "") == "butt") out.push_back({c.value("id", 0), c.value("text", "")});
    } catch (...) {}
    return out;
}

// The saves the load menu lists: G_LoadSaveClass + 0x14 / + 0x18, records of 24 bytes ({?, 16, number, ?, ?, text
// id}: SaveGameEnumFiles 0x605B20 reads them from savegame\mafiaNNN.<number>); the menu shows them newest first,
// the k-th record as the row 100 + (count - 1 - k) (its command handler 0x5E2840 selects count + 99 - row).
const char *TextOrNull(unsigned id) {
    using GetFn = const char *(__fastcall *)(uintptr_t db, void *edx, unsigned id);
    const game::ExeBuild &e = game::Exe();
    __try { return reinterpret_cast<GetFn>(e.textGetOrNull)(e.textDb, nullptr, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

struct Save { int number; std::string name; };
std::vector<Save> Saves() {
    std::vector<Save> out;
    uintptr_t ls = game::Exe().loadSave;
    uint32_t b = Rd(ls + 0x14), e = Rd(ls + 0x18);
    if (!b || e < b || (e - b) % 24 || (e - b) / 24 > 1000) return out;
    for (uint32_t r = b; r < e; r += 24) {
        const char *t = game::Exe().textGetOrNull ? TextOrNull(Rd(r + 0x14)) : nullptr;
        char buf[128] = {};
        if (t) MemRead(reinterpret_cast<uintptr_t>(t), buf, sizeof buf - 1);
        out.push_back({static_cast<int>(Rd(r + 8)), MenuUtf8(buf)});
    }
    return out;
}

std::string SaveList(const std::vector<Save> &saves, size_t max) {
    std::string s;
    for (size_t i = saves.size(); i-- > 0 && saves.size() - i <= max;) s += (s.empty() ? "" : ", ") + std::to_string(saves[i].number) + " \"" + saves[i].name + "\"";
    return s;
}

void Step() {
    uintptr_t top = TopMenu();
    if (!top) return;
    uintptr_t vt = MenuClassOf(top);
    const game::ExeBuild &e = game::Exe();
    if (vt == e.menuProfileVt && !g_req.profileDone && (g_req.profileNumber >= 0 || !g_req.profileName.empty())) {
        std::vector<Profile> list = Profiles();
        int idx = -1;
        for (size_t i = 0; i < list.size(); i++)
            if (g_req.profileName.empty() ? list[i].number == g_req.profileNumber : !_stricmp(list[i].name.c_str(), g_req.profileName.c_str()))
                idx = static_cast<int>(i);
        if (idx < 0) return Fail("no profile " + (g_req.profileName.empty() ? std::to_string(g_req.profileNumber) : "'" + g_req.profileName + "'") +
                                 " (" + std::to_string(list.size()) + " profiles; menu.state lists them)");
        HLOG("start: profile %d (number %d) taken", idx, list[idx].number);
        g_req.profileDone = true;
        g_req.state = "profile";
        if (!SafeCommand(top, 100 + idx)) Fail("the profile menu's command faulted");
        return;
    }
    if (vt == e.menuMainVt && !g_req.mainDone) {
        // the main menu's commands (its handler 0x5E06E0): 990105 the freeride menu, 990104 the load menu, 990103 a
        // new game (MafiaEntryPoint plays the intro mission, then mise01), 990106 the freeride extreme (27)
        static const struct { const char *go; int command; } kMain[] = {{"freeride", 990105}, {"load", 990104}, {"new", 990103}, {"extreme", 990106}};
        g_req.mainDone = true;
        for (auto &m : kMain) {
            if (g_req.go != m.go) continue;
            HLOG("start: main menu -> %s", m.go);
            g_req.state = "main";
            if (!SafeCommand(top, m.command)) return Fail("the main menu's command faulted");
            if (g_req.go == "new" || g_req.go == "extreme") Done(g_req.go == "new" ? "a new game started" : "the freeride extreme started");
            return;
        }
        return Done("in the main menu");
    }
    if (vt == e.menuFreerideVt && g_req.go == "freeride" && !g_req.freerideDone) {
        if (g_req.kind && !g_req.kindAsked) {   // the kind menu first (the freeride menu's command 100)
            g_req.kindAsked = true;
            g_req.state = "freeride";
            HLOG("start: freeride menu -> its kind menu");
            if (!SafeCommand(top, 100)) Fail("the freeride menu's command faulted");
            return;
        }
        if (g_req.kind && !g_req.kindChosen) return;   // (the kind menu is still to be seen)
        g_req.freerideDone = true;
        static const char *kDensity[3] = {"traffic", "pedestrians", "police"};
        for (int i = 0; i < 3; i++) {
            if (g_req.density[i] < 0) continue;
            if (!SafeSetValue(top, 21 + i, g_req.density[i])) return Fail(std::string("the freeride menu's ") + kDensity[i] + " slider cannot be set");
            HLOG("start: freeride menu: %s %.2f", kDensity[i], g_req.density[i]);
            if (Rd(e.loadSave + 0x1F0) < 3)
                HWARN("start: this profile has not opened the freeride's settings: the game will not use the %s density", kDensity[i]);
        }
        uint32_t kind = Rd(top + e.freerideKind);
        if (g_req.kind && kind != static_cast<uint32_t>(g_req.kind->result))
            return Fail("the freeride menu's kind is " + std::to_string(kind) + " after the kind menu, not " + std::to_string(g_req.kind->result));
        HLOG("start: freeride menu -> start (kind %u)", kind);
        if (!SafeCommand(top, 201)) return Fail("the freeride menu's command faulted");
        if (kind == 28) return Done("freeride started");   // (other kinds open the spawn menu: "freeridestart")
        g_req.state = "place";
        return;
    }
    std::string name = MenuDefName(top);
    if (name == "freeridetrack" && g_req.kindAsked && !g_req.kindChosen) {
        // the kind menu (OnCreate 0x5E1DD0 hides the rows the profile's progress has not opened; a command reaches its
        // handler all the same: said in the log and in start.state)
        g_req.kindChosen = true;
        uint32_t level = Rd(e.loadSave + 0x1F0);
        if (static_cast<int>(level) < g_req.kind->unlock) {
            g_req.note = std::string("the profile has not opened the freeride '") + g_req.kind->name + "' (its menu hides the row): started all the same";
            HWARN("start: %s", g_req.note.c_str());
        }
        HLOG("start: kind menu -> %s (%s)", g_req.kind->name, g_req.kind->mission);
        if (!SafeCommand(top, g_req.kind->command)) Fail("the kind menu's command faulted");
        return;
    }
    if (name == "freeridestart" && g_req.go == "freeride" && g_req.freerideDone && !g_req.done) {
        // the spawn menu (its handler 0x5E1F90: rows 101.. a place, stored by FreerideStoreB, then the freeride)
        std::vector<std::pair<int, std::string>> rows = Rows(top), places;
        for (auto &r : rows) if (r.first > 100 && r.first < 108) places.push_back(r);
        int id = 0;
        if (!g_req.placeName.empty()) {
            for (auto &p : places) if (Lower(p.second).find(Lower(g_req.placeName)) != std::string::npos) { id = p.first; break; }
        } else {
            id = 100 + (g_req.placeNumber ? g_req.placeNumber : 1);
            bool listed = false;
            for (auto &p : places) listed |= p.first == id;
            if (!listed) id = 0;
        }
        if (!id) {
            std::string list;
            for (auto &p : places) list += (list.empty() ? "" : ", ") + std::to_string(p.first - 100) + " \"" + p.second + "\"";
            return Fail("no place " + (g_req.placeName.empty() ? std::to_string(g_req.placeNumber) : "\"" + g_req.placeName + "\"") + " in the spawn menu (" + list + ")");
        }
        HLOG("start: spawn menu -> place %d", id - 100);
        if (!SafeCommand(top, id)) return Fail("the spawn menu's command faulted");
        return Done("freeride started");
    }
    if (name == "loadgame" && g_req.go == "load" && !g_req.done) {
        std::vector<Save> saves = Saves();
        int k = -1;
        if (g_req.saveNumber >= 0) {
            for (size_t i = 0; i < saves.size(); i++) if (saves[i].number == g_req.saveNumber) k = static_cast<int>(i);
        } else {
            std::vector<int> hits;
            for (size_t i = 0; i < saves.size(); i++) if (Lower(saves[i].name).find(Lower(g_req.saveName)) != std::string::npos) hits.push_back(static_cast<int>(i));
            if (hits.size() > 1) {
                std::string list;
                for (int h : hits) list += (list.empty() ? "" : ", ") + std::to_string(saves[h].number) + " \"" + saves[h].name + "\"";
                return Fail("\"" + g_req.saveName + "\" names several saves: " + list);
            }
            if (hits.size() == 1) k = hits[0];
        }
        if (k < 0)
            return Fail("no save " + (g_req.saveName.empty() ? std::to_string(g_req.saveNumber) : "\"" + g_req.saveName + "\"") + " of this profile (" +
                        std::to_string(saves.size()) + " saves; the newest: " + SaveList(saves, 8) + ")");
        int row = 100 + (static_cast<int>(saves.size()) - 1 - k);
        HLOG("start: load menu -> save %d \"%s\" (row %d)", saves[k].number, saves[k].name.c_str(), row);
        if (!SafeCommand(top, row)) return Fail("the load menu's command faulted");
        return Done("the save is loading");
    }
}

json MenuState() {
    json out = {{"menu_known", g_menuOk}};
    if (!g_menuOk) return out;
    json stack = json::array();
    for (uintptr_t m = TopMenu(), n = 0; m && n < 16; m = UnderMenu(m), n++) {
        uintptr_t vt = MenuClassOf(m);   // (the menu's own class: the loader may have put a copy of its vtable there)
        char a[16], v[16];
        snprintf(a, sizeof a, "0x%08X", static_cast<unsigned>(m));
        snprintf(v, sizeof v, "0x%06X", static_cast<unsigned>(vt));
        // what a click's id reaches: 1.2's command handler, 1.0's screen tick (it switches on the id)
        json entry = {{"menu", a}, {"vtable", v}, {"kind", MenuName(vt)}};
        entry[m10::Known() ? "tick" : "command_handler"] = DescribeAddress(Rd(vt + (m10::Known() ? 0x14 : 0x24)));
        try {
            json d = json::parse(MenuDescribe(m));   // its definition's name, its controls (menus.cpp)
            entry["name"] = d["name"];
            entry["controls"] = d["controls"];
            if (d.contains("page_of")) entry["page_of"] = d["page_of"];
        } catch (...) {}
        stack.push_back(entry);
    }
    out["menus"] = stack;   // the active one first
    json profiles = json::array();
    for (const Profile &p : Profiles()) profiles.push_back({{"number", p.number}, {"name", p.name}});
    out["profiles"] = profiles;   // the list the game has read (while the profile menu is open)
    return out;
}

} // namespace

void StartInit() {
    if (!game::g_exe || !game::Exe().menuTop || !game::Exe().menuProfileVt) return;
    g_menuOk = true;
    std::string path = MmlDir() + "start.json";
    std::ifstream f(Wide(path));
    if (!f) return;
    json r;
    try { r = json::parse(f, nullptr, true, true); } catch (const std::exception &ex) { HWARN("start.json: %s", ex.what()); }
    f.close();
    PathDelete(path);   // asked for once: the next launch is the usual one
    if (!r.is_object()) return;
    g_req.active = true;
    g_req.started = GetTickCount();
    g_req.state = "waiting";
    g_req.skipLogos = r.value("skip_logos", true);
    if (r.contains("profile")) {
        if (r["profile"].is_number_integer()) g_req.profileNumber = r["profile"].get<int>();
        else if (r["profile"].is_string()) g_req.profileName = r["profile"].get<std::string>();
    }
    g_req.go = r.value("go", "");
    if (r.value("background", false)) KeepInBackground();
    if (r.contains("resolution")) {
        const json &res = r["resolution"];
        if (res.is_array() && res.size() == 2 && res[0].is_number_unsigned() && res[1].is_number_unsigned() &&
            res[0].get<unsigned>() >= 320 && res[1].get<unsigned>() >= 240 && res[0].get<unsigned>() <= 16384 && res[1].get<unsigned>() <= 16384)
            AskResolution(res[0].get<unsigned>(), res[1].get<unsigned>());
        else
            HWARN("start.json: resolution: [width, height], 320x240 to 16384x16384");
    }
    if (r.contains("fullscreen")) {
        if (r["fullscreen"].is_boolean() && TakeSetupRead()) g_fullAsked = r["fullscreen"].get<bool>() ? 1 : 0;
        else if (!r["fullscreen"].is_boolean()) HWARN("start.json: fullscreen: true or false");
    }
    if (r.contains("adapter")) {
        if (r["adapter"].is_number_unsigned() && r["adapter"].get<unsigned>() < 16 && TakeSetupRead()) g_adapterAsked = r["adapter"].get<int>();
        else if (!r["adapter"].is_number_unsigned() || r["adapter"].get<unsigned>() >= 16) HWARN("start.json: adapter: Direct3D's ordinal, 0 to 15");
    }
    if (r.contains("kind") && r["kind"].is_string()) {
        std::string k = Lower(r["kind"].get<std::string>());
        for (const Kind &kd : kKinds) if (k == kd.name || k == kd.alias || k == kd.mission) g_req.kind = &kd;
        if (!g_req.kind) Fail("kind \"" + k + "\": city-small, city-night, countryside-night, city, countryside (or the missions' names)");
        else if (g_req.kind->result == 28) g_req.kind = nullptr;   // (the menu's own)
    }
    if (r.contains("place")) {
        if (r["place"].is_number_integer()) g_req.placeNumber = r["place"].get<int>();
        else if (r["place"].is_string()) g_req.placeName = r["place"].get<std::string>();
    }
    if (r.contains("save")) {
        if (r["save"].is_number_integer()) g_req.saveNumber = r["save"].get<int>();
        else if (r["save"].is_string()) g_req.saveName = r["save"].get<std::string>();
    }
    if (g_req.go == "load" && g_req.saveNumber < 0 && g_req.saveName.empty()) Fail("go \"load\": save, a save's number or a part of its name");
    if (!g_req.go.empty() && g_req.go != "menu" && g_req.go != "freeride" && g_req.go != "load" && g_req.go != "new" && g_req.go != "extreme")
        Fail("go \"" + g_req.go + "\": menu, freeride, load, new, extreme");
    static const char *kDensity[3] = {"traffic", "pedestrians", "police"};
    for (int i = 0; i < 3; i++)
        if (r.contains(kDensity[i]) && r[kDensity[i]].is_number()) {
            float v = r[kDensity[i]].get<float>();
            g_req.density[i] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
    if (g_req.skipLogos && game::Exe().playVideo) {
        std::string err;
        if (!HookAdd(nullptr, game::Exe().playVideo, reinterpret_cast<void *>(&PlayVideoHook), reinterpret_cast<void **>(&g_playVideo), &err))
            HWARN("start: cannot hook PlayVideo: %s", err.c_str());
    }
    if (g_req.skipLogos && game::Exe().m10CdFind) {
        std::string err;
        if (!HookAdd(nullptr, game::Exe().m10CdFind, reinterpret_cast<void *>(&CdFindHook), reinterpret_cast<void **>(&g_cdFind), &err))
            HWARN("start: cannot hook cd_find_game_file: %s", err.c_str());
    }
    HLOG("start asked: skip_logos %d, profile %s, go '%s'", g_req.skipLogos,
         g_req.profileName.empty() ? std::to_string(g_req.profileNumber).c_str() : g_req.profileName.c_str(), g_req.go.c_str());
}

void StartFrame() {
    if (g_req.active) Step();
}

// menu.state, menu.command {command}, menu.value {id, value}, start.state; game thread. False: not one of these.
bool StartMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "menu.state" && method != "menu.command" && method != "menu.value" && method != "start.state") return false;
    *ok = true;
    if (method == "start.state") {
        json s = {{"state", g_req.state}, {"error", g_req.error}, {"active", g_req.active}, {"go", g_req.go}};
        if (!g_req.note.empty()) s["note"] = g_req.note;
        if (g_req.kind) s["kind"] = g_req.kind->name;
        *reply = s.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (!g_menuOk) { *ok = false; *reply = "the menus are not known for this build"; return true; }
    if (method == "menu.state") { *reply = MenuState().dump(-1, ' ', false, json::error_handler_t::replace); return true; }
    json p;
    try { p = json::parse(paramsJson.empty() ? "{}" : paramsJson); } catch (...) {}
    uintptr_t top = TopMenu();
    if (!top) { *ok = false; *reply = "no menu is open"; return true; }
    if (method == "menu.value") {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_number_integer()) { *ok = false; *reply = "id: a component's id (an integer); value: to set it"; return true; }
        int id = p["id"].get<int>();
        float value = 0;
        if (p.contains("value")) {
            if (!p["value"].is_number()) { *ok = false; *reply = "value: a number"; return true; }
            HLOG("menu.value %d = %g in the menu '%s' 0x%08X", id, p["value"].get<float>(), MenuDefName(top).c_str(), static_cast<unsigned>(top));
            if (!SafeSetValue(top, id, p["value"].get<float>())) { *ok = false; *reply = "the component's setter is not known for this build, or faulted"; return true; }
        }
        if (!MenuControlValue(top, id, &value) && !SafeGetValue(top, id, &value)) { *ok = false; *reply = "the component's getter is not known for this build, or faulted"; return true; }
        *reply = json{{"id", id}, {"value", value}}.dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (!p.is_object() || !p.contains("command") || !p["command"].is_number_integer()) { *ok = false; *reply = "command: an integer"; return true; }
    int cmd = p["command"].get<int>();
    HLOG("menu.command %d to the %s menu 0x%08X", cmd, MenuName(MenuClassOf(top)), static_cast<unsigned>(top));
    if (!SafeCommand(top, cmd)) { *ok = false; *reply = "the menu's command faulted"; return true; }
    json state = MenuState();
    if (m10::Known()) state["note"] = "1.0: the command is the screen's next tick's answer (as a click there): menu.state shows what it did";
    *reply = state.dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

} // namespace host
