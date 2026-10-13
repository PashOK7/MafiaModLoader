// The start: the game taken from its launch to a chosen profile and freeride by its own menus, and the menus driven
// by agents (bridge methods menu.state, menu.command, start.state).
//
// A start is asked for once, by <game>\mml\start.json (the MCP server's game_launch writes it; it is deleted when
// read, so the next launch is the game's usual one):
//   {"skip_logos": true, "profile": 4 | "name", "go": "menu" | "freeride", "traffic": 1, "pedestrians": 1, "police": 0}
// - skip_logos: the three logo videos InitSystem plays before the profile menu are not played (PlayVideo answers
//   "played" for logo1, logo2, logo3 only: the videos missions play are left alone);
// - profile: in the profile menu, the profile with this number (savegame\mafiaNNN.sav) or name is taken, by the
//   command a click on it sends (100 + its place in the list the game has read);
// - go: "freeride" sends the main menu's freeride command, then the freeride menu's start: the game's default
//   freeride with the menu's own default settings (missions\freeitaly). "menu" stops in the main menu;
// - traffic, pedestrians, police (0..1; each optional): the freeride menu's three density sliders, set before its
//   start by the setter the menu itself sets them with ("police": 0 is a city without patrols). The game uses
//   them only for a profile that has opened the freeride's settings; the log says when it has not.
// The bridge's menu.value {id, value} reads (and sets) such a component of the active menu.
// Each step waits for its menu to be the active one and sends one command, as a click would, from the frame
// limiter's call inside the menu's loop (the loop the click handlers run in). The menus' addresses: game_build.h.
#include "host.h"
#include <fstream>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

struct Request {
    bool active = false, skipLogos = false;
    int profileNumber = -1;        // -1: by name, or none
    std::string profileName;       // empty: by number, or none
    std::string go;                // "", "menu", "freeride"
    float density[3] = {-1, -1, -1};   // the freeride's traffic, pedestrians, police (below 0: the menu's own)
    bool profileDone = false, mainDone = false, freerideDone = false, done = false;
    std::string state = "none", error;
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

uint32_t Rd(uintptr_t a) {
    uint32_t v = 0;
    MemRead(a, &v, 4);
    return v;
}

uintptr_t TopMenu() { return g_menuOk ? Rd(game::Exe().menuTop) : 0; }

const char *MenuName(uintptr_t vt) {
    const game::ExeBuild &e = game::Exe();
    if (vt == e.menuProfileVt) return "profile";
    if (vt == e.menuMainVt) return "main";
    if (vt == e.menuFreerideVt) return "freeride";
    return "";
}

// A menu's command, as a click sends it: its vtable's +0x24, thiscall(menu, command). Game thread only.
using CommandFn = int(__fastcall *)(uintptr_t menu, void *edx, int command);
bool SafeCommand(uintptr_t menu, int command) {
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
        g_req.mainDone = true;
        if (g_req.go == "freeride") {
            HLOG("start: main menu -> freeride");
            g_req.state = "main";
            if (!SafeCommand(top, 990105)) Fail("the main menu's command faulted");
        } else {
            g_req.state = "done";
            g_req.done = true;
            g_req.active = false;
            HLOG("start: in the main menu (%.1f s after the launch)", (GetTickCount() - g_req.started) / 1000.0);
        }
        return;
    }
    if (vt == e.menuFreerideVt && g_req.go == "freeride" && !g_req.freerideDone) {
        g_req.freerideDone = true;
        static const char *kDensity[3] = {"traffic", "pedestrians", "police"};
        for (int i = 0; i < 3; i++) {
            if (g_req.density[i] < 0) continue;
            if (!SafeSetValue(top, 21 + i, g_req.density[i])) return Fail(std::string("the freeride menu's ") + kDensity[i] + " slider cannot be set");
            HLOG("start: freeride menu: %s %.2f", kDensity[i], g_req.density[i]);
            if (Rd(e.loadSave + 0x1F0) < 3)
                HWARN("start: this profile has not opened the freeride's settings: the game will not use the %s density", kDensity[i]);
        }
        HLOG("start: freeride menu -> start (kind %u)", Rd(top + 0x30));
        if (!SafeCommand(top, 201)) return Fail("the freeride menu's command faulted");
        g_req.state = "done";
        g_req.done = true;
        g_req.active = false;
        HLOG("start: freeride started (%.1f s after the launch)", (GetTickCount() - g_req.started) / 1000.0);
    }
}

json MenuState() {
    json out = {{"menu_known", g_menuOk}};
    if (!g_menuOk) return out;
    json stack = json::array();
    for (uintptr_t m = TopMenu(), n = 0; m && n < 16; m = Rd(m + 4), n++) {
        uintptr_t vt = MenuClassOf(m);   // (the menu's own class: the loader may have put a copy of its vtable there)
        char a[16], v[16];
        snprintf(a, sizeof a, "0x%08X", static_cast<unsigned>(m));
        snprintf(v, sizeof v, "0x%06X", static_cast<unsigned>(vt));
        json entry = {{"menu", a}, {"vtable", v}, {"kind", MenuName(vt)}, {"command_handler", DescribeAddress(Rd(vt + 0x24))}};
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
    std::ifstream f(path);
    if (!f) return;
    json r;
    try { r = json::parse(f, nullptr, true, true); } catch (const std::exception &ex) { HWARN("start.json: %s", ex.what()); }
    f.close();
    DeleteFileA(path.c_str());   // asked for once: the next launch is the usual one
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
    static const char *kDensity[3] = {"traffic", "pedestrians", "police"};
    for (int i = 0; i < 3; i++)
        if (r.contains(kDensity[i]) && r[kDensity[i]].is_number()) {
            float v = r[kDensity[i]].get<float>();
            g_req.density[i] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
    if (g_req.skipLogos) {
        std::string err;
        if (!HookAdd(nullptr, game::Exe().playVideo, reinterpret_cast<void *>(&PlayVideoHook), reinterpret_cast<void **>(&g_playVideo), &err))
            HWARN("start: cannot hook PlayVideo: %s", err.c_str());
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
        *reply = json{{"state", g_req.state}, {"error", g_req.error}, {"active", g_req.active}, {"go", g_req.go}}.dump(-1, ' ', false, json::error_handler_t::replace);
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
    *reply = MenuState().dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

} // namespace host
