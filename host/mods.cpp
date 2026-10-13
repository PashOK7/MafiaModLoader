// Mods: loading, the host table, events, commands, services, state (see host.h, mml.h, docs/MODDING.md).
//
// A mod lives in <game>\mml\mods\<id>\ with a mod.json manifest:
//   { "id": "core", "name": "...", "version": "0.1.0", "type": "native", "entry": "core.dll",
//     "enabled": true, "load_order": 0, "depends": [] }
// A native mod's DLL is copied to <game>\mml\live\<id>_<pid>_<n>.dll and loaded from there, so the original can be
// rebuilt while the game runs; `mod.reload` unloads it (everything it registered goes with it) and loads the new
// copy. A mod that faults inside one of its callbacks is unloaded at the next frame and logged; the game goes on.
#include "host.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include "json.hpp"

using json = nlohmann::json;

struct ModEntry;
struct MmlMod {             // one load of a mod (a reload makes a new one; old ones are kept, dead, never freed)
    ModEntry *entry;
    bool alive;
};
struct MmlReply { std::string text; bool error = false; };

struct ModEntry {
    std::string id, name, version, type, entry, dir, error;
    bool enabled = true;
    int order = 100;
    std::vector<std::string> depends;
    HMODULE dll = nullptr;
    MmlModUnloadFn unload = nullptr;
    MmlMod *current = nullptr;        // the live load, or null
    unsigned generation = 0;
    std::string livePath;
    bool faulted = false;             // a callback faulted: unloaded at the next frame
    std::map<std::string, std::vector<uint8_t>> state;
};

namespace host {
namespace {
std::vector<std::unique_ptr<ModEntry>> g_mods;
struct Sub { MmlMod *mod; MmlEventType type; MmlEventFn fn; void *user; };
std::vector<Sub> g_subs;
struct Command { MmlMod *mod; std::string help; MmlCommandFn fn; void *user; };
std::map<std::string, Command> g_commands;
struct Service { MmlMod *mod; uint32_t version; const void *table; };
std::map<std::string, Service> g_services;
struct Posted { MmlMod *mod; void (*fn)(void *); void *user; };
std::mutex g_postMx;
std::vector<Posted> g_posted;
MmlHost g_host;

ModEntry *ById(const std::string &id) {
    for (auto &m : g_mods) if (m->id == id) return m.get();
    return nullptr;
}
bool Alive(const MmlMod *m) { return m && m->alive; }
const char *Who(const MmlMod *m) { return m ? m->entry->id.c_str() : "?"; }

// ---- calls into mods, each behind __try: a fault marks the mod for unloading instead of killing the game
void Faulted(MmlMod *m, const char *where, DWORD code) {
    if (!m || m->entry->faulted) return;
    m->entry->faulted = true;
    Log(MML_LOG_ERROR, "host", "mod %s faulted (exception %08X) in %s: unloaded at the next frame", Who(m),
        static_cast<unsigned>(code), where);
}
DWORD SafeEvent(MmlEventFn fn, const MmlEvent *ev, void *user) {
    __try { fn(ev, user); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafeCommand(MmlCommandFn fn, const char *args, MmlReply *r, void *user, int *rc) {
    __try { *rc = fn(args, r, user); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafeLoad(MmlModLoadFn fn, const MmlHost *h, MmlMod *m, int *rc) {
    __try { *rc = fn(h, m); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafeUnload(MmlModUnloadFn fn, MmlMod *m) {
    __try { fn(m); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafePosted(void (*fn)(void *), void *user) {
    __try { fn(user); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

void ServicesChanged(const std::string &name) {
    MmlEvent ev{};
    ev.size = sizeof ev;
    ev.type = MML_EVENT_SERVICES_CHANGED;
    ev.name = name.c_str();
    Dispatch(ev);
}

// ---- the host table (mml.h)
const char *H_GameBuild() { return GameBuild(); }
const char *H_GameDir() { return GameDir().c_str(); }
const char *H_ModId(const MmlMod *m) { return m ? m->entry->id.c_str() : ""; }
const char *H_ModDir(const MmlMod *m) { return m ? m->entry->dir.c_str() : ""; }
void H_Log(const MmlMod *m, MmlLogLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(level, Who(m), fmt, ap);
    va_end(ap);
}
int H_MemRead(uintptr_t a, void *d, size_t n) { return MemRead(a, d, n) ? 1 : 0; }
int H_MemWrite(uintptr_t a, const void *s, size_t n) { return MemWrite(a, s, n) ? 1 : 0; }
uintptr_t H_ModuleBase(const char *name) { return ModuleBase(name); }
int H_Hook(MmlMod *m, uintptr_t target, void *detour, void **original) {
    if (!Alive(m)) return 0;
    std::string err;
    if (HookAdd(m, target, detour, original, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "hook %s failed: %s", DescribeAddress(target).c_str(), err.c_str());
    return 0;
}
int H_Unhook(MmlMod *m, uintptr_t target) { return Alive(m) && HookRemove(m, target) ? 1 : 0; }
int H_Subscribe(MmlMod *m, MmlEventType type, MmlEventFn fn, void *user) {
    if (!Alive(m) || !fn || type <= 0 || type >= MML_EVENT_COUNT_) return 0;
    g_subs.push_back(Sub{m, type, fn, user});
    return 1;
}
int H_Command(MmlMod *m, const char *name, const char *help, MmlCommandFn fn, void *user) {
    if (!Alive(m) || !name || !*name || !fn) return 0;
    std::string full = m->entry->id == "core" ? std::string(name) : m->entry->id + "." + name;
    g_commands[full] = Command{m, help ? help : "", fn, user};
    return 1;
}
void H_ReplyAppend(MmlReply *r, const char *text, size_t len) {
    if (!r || !text) return;
    r->text.append(text, len == static_cast<size_t>(-1) ? strlen(text) : len);
}
void H_ReplyError(MmlReply *r, const char *msg) {
    if (!r) return;
    r->error = true;
    r->text = msg ? msg : "error";
}
int H_StatePut(MmlMod *m, const char *key, const void *data, size_t n) {
    if (!m || !key) return 0;
    auto &blob = m->entry->state[key];
    blob.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + (data ? n : 0));
    return 1;
}
size_t H_StateGet(MmlMod *m, const char *key, void *data, size_t cap) {
    if (!m || !key) return 0;
    auto it = m->entry->state.find(key);
    if (it == m->entry->state.end()) return 0;
    if (data) memcpy(data, it->second.data(), std::min(cap, it->second.size()));
    return it->second.size();
}
int H_ServicePublish(MmlMod *m, const char *name, uint32_t version, const void *table) {
    if (!Alive(m) || !name || !table) return 0;
    g_services[name] = Service{m, version, table};
    Log(MML_LOG_INFO, Who(m), "service %s v%u published", name, version);
    ServicesChanged(name);
    return 1;
}
const void *H_ServiceFind(const char *name, uint32_t minVersion, uint32_t *version) {
    auto it = name ? g_services.find(name) : g_services.end();
    if (it == g_services.end() || it->second.version < minVersion || !Alive(it->second.mod)) return nullptr;
    if (version) *version = it->second.version;
    return it->second.table;
}
void H_Post(MmlMod *m, void (*fn)(void *), void *user) {
    if (!fn) return;
    std::lock_guard<std::mutex> lk(g_postMx);
    g_posted.push_back(Posted{m, fn, user});
}
// (the kit: drawing, keys and actions, the mission, files and settings: draw.cpp, input.cpp, game_hooks.cpp, config.cpp)
void H_DrawText(MmlMod *m, float x, float y, float size, uint32_t argb, int font, int align, const char *text) {
    if (Alive(m)) DrawText(m, x, y, size, argb, font, align, text);
}
float H_DrawTextWidth(int font, float size, const char *text) { return DrawTextWidth(font, size, text); }
void H_DrawLine(MmlMod *m, float x0, float y0, float x1, float y1, uint32_t argb) { if (Alive(m)) DrawLine(m, x0, y0, x1, y1, argb); }
void H_DrawRect(MmlMod *m, float x, float y, float w, float h, uint32_t argb, int filled) { if (Alive(m)) DrawRect(m, x, y, w, h, argb, filled); }
int H_KeyCode(const char *name) { return KeyCodeOf(name); }
int H_KeyDown(int key) { return KeyDown(key) ? 1 : 0; }
int H_KeyPressed(int key) { return KeyPressed(key) ? 1 : 0; }
int H_ActionSlot(const char *name) { return ActionSlotOf(name); }
float H_ActionHeld(int slot) { return ActionValue(slot, false); }
float H_ActionPressed(int slot) { return ActionValue(slot, true); }
int H_Hotkey(MmlMod *m, const char *combo, MmlHotkeyFn fn, void *user) {
    if (HotkeyAdd(m, combo, fn, user)) return 1;
    Log(MML_LOG_WARN, Who(m), "hotkey '%s' is not understood (\"F5\", \"Ctrl+F5\", \"Ctrl+Shift+R\")", combo ? combo : "");
    return 0;
}
const char *H_MissionName() { return MissionName(); }
int H_MissionState() { return MissionState(); }
const char *H_ModDataDir(MmlMod *m) { return Alive(m) ? ModDataDir(m) : ""; }
double H_ConfigNumber(MmlMod *m, const char *key, double def) { return ConfigNumber(m, key, def); }
const char *H_ConfigString(MmlMod *m, const char *key, const char *def) { return ConfigString(m, key, def); }
const char *H_ConfigJson(MmlMod *m, const char *key) { return ConfigJson(m, key); }
int H_ConfigSet(MmlMod *m, const char *key, const char *value) { return ConfigSet(m, key, value) ? 1 : 0; }
int H_MenuItem(MmlMod *m, const char *menu, int after, const char *text, MmlMenuFn fn, void *user) {
    return Alive(m) ? MenuItemAdd(m, menu, after, text, fn, user) : 0;
}
int H_MenuOpen(MmlMod *m, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user) {
    return Alive(m) ? MenuPageOpen(m, name, controls, count, fn, user) : 0;
}
void H_MenuClose(MmlMod *m) { if (Alive(m)) MenuPageClose(m); }
int H_MenuSetText(MmlMod *m, int id, const char *text) { return Alive(m) ? MenuPageText(m, id, text) : 0; }
int H_MenuSetValue(MmlMod *m, int id, float value) { return Alive(m) ? MenuPageValue(m, id, value) : 0; }
int H_MenuValue(MmlMod *m, int id, float *value) { return Alive(m) ? MenuPageGetValue(m, id, value) : 0; }
int H_MenuSetRange(MmlMod *m, int id, int lo, int hi) { return Alive(m) ? MenuPageRange(m, id, lo, hi) : 0; }
int H_MenuText(MmlMod *m, int id, char *buf, int size) { return Alive(m) ? MenuPageGetText(m, id, buf, size) : 0; }

// ---- manifests
bool ReadManifest(const std::string &dir, const std::string &folder, ModEntry *e, std::string *err) {
    std::ifstream f(dir + "mod.json");
    if (!f) { *err = "no mod.json"; return false; }
    try {
        json j = json::parse(f, nullptr, true, true);   // comments allowed
        e->id = j.value("id", folder);
        e->name = j.value("name", e->id);
        e->version = j.value("version", "0.0.0");
        e->type = j.value("type", "native");
        e->entry = j.value("entry", e->id + ".dll");
        e->enabled = j.value("enabled", true);
        e->order = j.value("load_order", 100);
        e->depends.clear();
        if (j.contains("depends") && j["depends"].is_array())
            for (auto &d : j["depends"]) if (d.is_string()) e->depends.push_back(d.get<std::string>());
        e->dir = dir;
        return true;
    } catch (const std::exception &ex) {
        *err = std::string("mod.json: ") + ex.what();
        return false;
    }
}

bool UnloadMod(ModEntry *e, const char *why);

bool LoadMod(ModEntry *e, std::string *err) {
    if (e->current) { *err = "already loaded"; return false; }
    for (const std::string &d : e->depends) {
        ModEntry *dep = ById(d);
        if (!dep || !dep->current) { *err = "depends on " + d + ", which is not loaded"; return false; }
    }
    if (e->type != "native") { *err = "type " + e->type + " has no runtime yet"; return false; }
    std::string src = e->dir + e->entry;
    std::string live = MmlDir() + "live\\";
    CreateDirectoryA(live.c_str(), nullptr);
    char name[128];
    snprintf(name, sizeof name, "mod_%s_%u_%u.dll", e->id.c_str(), static_cast<unsigned>(GetCurrentProcessId()), ++e->generation);
    std::string path = live + name;
    if (!CopyFileA(src.c_str(), path.c_str(), FALSE)) { *err = "cannot copy " + src; return false; }
    HMODULE dll = LoadLibraryA(path.c_str());
    if (!dll) { *err = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")"; DeleteFileA(path.c_str()); return false; }
    auto load = reinterpret_cast<MmlModLoadFn>(GetProcAddress(dll, "mml_mod_load"));
    auto unload = reinterpret_cast<MmlModUnloadFn>(GetProcAddress(dll, "mml_mod_unload"));
    auto api = reinterpret_cast<MmlModApiFn>(GetProcAddress(dll, "mml_mod_api"));
    uint32_t want = api ? api() : MML_API_VERSION;
    if (!load || want > MML_API_VERSION) {
        FreeLibrary(dll);
        DeleteFileA(path.c_str());
        *err = !load ? "no mml_mod_load export" : "made for a newer loader (API " + std::to_string(want) + ")";
        return false;
    }
    MmlMod *m = new MmlMod{e, true};
    e->dll = dll;
    e->unload = unload;
    e->current = m;
    e->livePath = path;
    e->faulted = false;
    int rc = 0;
    DWORD ex = SafeLoad(load, &g_host, m, &rc);
    if (ex || rc != 0) {
        *err = ex ? "mml_mod_load faulted" : "mml_mod_load returned " + std::to_string(rc);
        e->unload = nullptr;   // (a failed load is not unloaded by the mod; what it registered goes all the same)
        UnloadMod(e, "its load failed");
        return false;
    }
    Log(MML_LOG_INFO, "host", "mod %s %s loaded (generation %u)", e->id.c_str(), e->version.c_str(), e->generation);
    MmlEvent ev{};
    ev.size = sizeof ev;
    ev.type = MML_EVENT_MOD_LOADED;
    ev.name = e->id.c_str();
    Dispatch(ev);
    return true;
}

bool UnloadMod(ModEntry *e, const char *why) {
    MmlMod *m = e->current;
    if (!m) return false;
    if (e->unload && !e->faulted) {
        DWORD ex = SafeUnload(e->unload, m);
        if (ex) Log(MML_LOG_ERROR, "host", "mod %s faulted in mml_mod_unload (%08X)", e->id.c_str(), static_cast<unsigned>(ex));
    }
    m->alive = false;
    HookRemoveAll(m);
    HotkeysRemove(m);
    MenusRemove(m);
    DrawRemove(m);
    ConfigForget(m);
    g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(), [&](const Sub &s) { return s.mod == m; }), g_subs.end());
    for (auto it = g_commands.begin(); it != g_commands.end();) it = it->second.mod == m ? g_commands.erase(it) : std::next(it);
    std::vector<std::string> gone;
    for (auto it = g_services.begin(); it != g_services.end();) {
        if (it->second.mod == m) { gone.push_back(it->first); it = g_services.erase(it); }
        else ++it;
    }
    {
        std::lock_guard<std::mutex> lk(g_postMx);
        g_posted.erase(std::remove_if(g_posted.begin(), g_posted.end(), [&](const Posted &p) { return p.mod == m; }), g_posted.end());
    }
    FreeLibrary(e->dll);
    DeleteFileA(e->livePath.c_str());
    e->dll = nullptr;
    e->unload = nullptr;
    e->current = nullptr;
    Log(MML_LOG_INFO, "host", "mod %s unloaded (%s)", e->id.c_str(), why);
    for (const std::string &s : gone) ServicesChanged(s);
    return true;
}

// The mods folder read again: new mods added, manifests of unloaded ones refreshed. Loaded mods keep their manifest
// until they are reloaded.
void Scan() {
    std::string root = MmlDir() + "mods\\";
    CreateDirectoryA(root.c_str(), nullptr);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        std::string dir = root + fd.cFileName + "\\";
        ModEntry tmp;
        std::string err;
        if (!ReadManifest(dir, fd.cFileName, &tmp, &err)) {
            HWARN("mods\\%s: %s", fd.cFileName, err.c_str());
            continue;
        }
        ModEntry *e = ById(tmp.id);
        if (!e) {
            g_mods.push_back(std::make_unique<ModEntry>());
            e = g_mods.back().get();
            e->id = tmp.id;
        }
        if (e->current) continue;
        e->name = tmp.name; e->version = tmp.version; e->type = tmp.type; e->entry = tmp.entry; e->enabled = tmp.enabled;
        e->order = tmp.order; e->depends = tmp.depends; e->dir = tmp.dir;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::stable_sort(g_mods.begin(), g_mods.end(), [](const std::unique_ptr<ModEntry> &a, const std::unique_ptr<ModEntry> &b) {
        return a->order != b->order ? a->order < b->order : a->id < b->id;
    });
}

void LoadEnabled() {
    // in load order; a mod whose dependencies are not loaded yet waits for the next pass
    for (int pass = 0; pass < 4; pass++) {
        bool any = false;
        for (auto &e : g_mods) {
            if (!e->enabled || e->current) continue;
            std::string err;
            if (LoadMod(e.get(), &err)) { e->error.clear(); any = true; }
            else e->error = err;
        }
        if (!any) break;
    }
    for (auto &e : g_mods)
        if (e->enabled && !e->current) Log(MML_LOG_WARN, "host", "mod %s not loaded: %s", e->id.c_str(), e->error.c_str());
}

void CleanLive() {
    // copies left by earlier runs (this run's are deleted as they unload)
    std::string dir = MmlDir() + "live\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileA((dir + fd.cFileName).c_str());
    while (FindNextFileA(h, &fd));
    FindClose(h);
}

std::string ModJson(const ModEntry &e) {
    json j = {{"id", e.id}, {"name", e.name}, {"version", e.version}, {"type", e.type}, {"enabled", e.enabled},
              {"loaded", e.current != nullptr}, {"generation", e.generation}, {"load_order", e.order}, {"dir", e.dir},
              {"depends", e.depends}};
    if (!e.error.empty()) j["error"] = e.error;
    return j.dump();
}
} // namespace

const char *ModIdOf(const MmlMod *mod) { return mod ? mod->entry->id.c_str() : "host"; }
bool ModAlive(const MmlMod *mod) { return Alive(mod); }
MmlMod *ModById(const std::string &id) {
    ModEntry *e = ById(id);
    return e ? e->current : nullptr;
}
bool ModDirById(const std::string &id, std::string *dir) {
    ModEntry *e = ById(id);
    if (!e) { Scan(); e = ById(id); }
    if (!e) return false;
    *dir = e->dir;
    return true;
}
std::vector<MmlMod *> ModsLoaded() {
    std::vector<MmlMod *> out;
    for (auto &e : g_mods) if (e->current && !e->faulted) out.push_back(e->current);
    return out;
}
bool ModCall(MmlMod *mod, void (*fn)(void *), void *user, const char *where) {
    if (!Alive(mod) || mod->entry->faulted) return false;
    if (DWORD ex = SafePosted(fn, user)) { Faulted(mod, where, ex); return false; }
    return true;
}

void ModsInit() {
    g_host = MmlHost{};
    g_host.size = sizeof g_host;
    g_host.api_version = MML_API_VERSION;
    g_host.game_build = H_GameBuild;
    g_host.game_dir = H_GameDir;
    g_host.mod_id = H_ModId;
    g_host.mod_dir = H_ModDir;
    g_host.log = H_Log;
    g_host.mem_read = H_MemRead;
    g_host.mem_write = H_MemWrite;
    g_host.module_base = H_ModuleBase;
    g_host.hook = H_Hook;
    g_host.unhook = H_Unhook;
    g_host.subscribe = H_Subscribe;
    g_host.command = H_Command;
    g_host.reply_append = H_ReplyAppend;
    g_host.reply_error = H_ReplyError;
    g_host.state_put = H_StatePut;
    g_host.state_get = H_StateGet;
    g_host.service_publish = H_ServicePublish;
    g_host.service_find = H_ServiceFind;
    g_host.post = H_Post;
    g_host.draw_text = H_DrawText;
    g_host.draw_text_width = H_DrawTextWidth;
    g_host.draw_line = H_DrawLine;
    g_host.draw_rect = H_DrawRect;
    g_host.key_code = H_KeyCode;
    g_host.key_down = H_KeyDown;
    g_host.key_pressed = H_KeyPressed;
    g_host.action_slot = H_ActionSlot;
    g_host.action_held = H_ActionHeld;
    g_host.action_pressed = H_ActionPressed;
    g_host.hotkey = H_Hotkey;
    g_host.mission_name = H_MissionName;
    g_host.mission_state = H_MissionState;
    g_host.mod_data_dir = H_ModDataDir;
    g_host.config_number = H_ConfigNumber;
    g_host.config_string = H_ConfigString;
    g_host.config_json = H_ConfigJson;
    g_host.config_set = H_ConfigSet;
    g_host.menu_item = H_MenuItem;
    g_host.menu_open = H_MenuOpen;
    g_host.menu_close = H_MenuClose;
    g_host.menu_set_text = H_MenuSetText;
    g_host.menu_set_value = H_MenuSetValue;
    g_host.menu_value = H_MenuValue;
    g_host.menu_set_range = H_MenuSetRange;
    g_host.menu_text = H_MenuText;
    CleanLive();
    Scan();
    LoadEnabled();
}

void ModsShutdown() {
    for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it) UnloadMod(it->get(), "the game is closing");
}

void Dispatch(const MmlEvent &ev) {
    if (g_subs.empty()) return;
    std::vector<Sub> subs = g_subs;   // a handler may subscribe; a faulted mod is unloaded only at the next frame
    for (const Sub &s : subs) {
        if (s.type != ev.type || !Alive(s.mod) || s.mod->entry->faulted) continue;
        if (DWORD ex = SafeEvent(s.fn, &ev, s.user)) Faulted(s.mod, "an event handler", ex);
    }
}

void DispatchTo(MmlMod *mod, const MmlEvent &ev) {
    std::vector<Sub> subs = g_subs;
    for (const Sub &s : subs) {
        if (s.mod != mod || s.type != ev.type || !Alive(s.mod) || s.mod->entry->faulted) continue;
        if (DWORD ex = SafeEvent(s.fn, &ev, s.user)) Faulted(s.mod, "an event handler", ex);
    }
}

void RunPosted() {
    std::vector<Posted> run;
    {
        std::lock_guard<std::mutex> lk(g_postMx);
        run.swap(g_posted);
    }
    for (const Posted &p : run) {
        if (p.mod && (!Alive(p.mod) || p.mod->entry->faulted)) continue;
        if (DWORD ex = SafePosted(p.fn, p.user)) Faulted(p.mod, "a posted call", ex);
    }
    for (auto &e : g_mods)
        if (e->current && e->faulted) UnloadMod(e.get(), "it faulted");
}

bool ExecCommand(const std::string &method, const std::string &paramsJson, std::string *reply) {
    json params;
    try { params = paramsJson.empty() ? json() : json::parse(paramsJson); }
    catch (const std::exception &ex) { *reply = std::string("params: ") + ex.what(); return false; }
    auto idParam = [&]() -> std::string { return params.is_object() ? params.value("id", "") : std::string(); };
    bool ok;
    if (StartMethod(method, paramsJson, reply, &ok)) return ok;
    if (InputMethod(method, paramsJson, reply, &ok)) return ok;
    if (ScreenshotMethod(method, paramsJson, reply, &ok)) return ok;
    if (DrawMethod(method, paramsJson, reply, &ok)) return ok;
    if (ConfigMethod(method, paramsJson, reply, &ok)) return ok;
    if (MenusMethod(method, paramsJson, reply, &ok)) return ok;
    if (method == "mods.list") { *reply = ModsList(); return true; }
    if (method == "commands.list") { *reply = CommandsList(); return true; }
    if (method == "hooks.list") { *reply = HookList(); return true; }
    if (method == "mods.rescan") {
        Scan();
        LoadEnabled();
        *reply = ModsList();
        return true;
    }
    if (method == "mod.reload" || method == "mod.load" || method == "mod.unload") {
        std::string id = idParam();
        if (method == "mod.reload" && id == "*") {   // every loaded mod, in reverse then forward order
            std::vector<ModEntry *> loaded;
            for (auto &e : g_mods) if (e->current) loaded.push_back(e.get());
            for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) UnloadMod(*it, "reload");
            Scan();
            for (ModEntry *e : loaded) { std::string err; if (!LoadMod(e, &err)) e->error = err; }
            *reply = ModsList();
            return true;
        }
        ModEntry *e = ById(id);
        if (!e) { Scan(); e = ById(id); }
        if (!e) { *reply = "no mod '" + id + "' (mods.list)"; return false; }
        if (method != "mod.load") UnloadMod(e, method == "mod.reload" ? "reload" : "mod.unload");
        if (method != "mod.unload") {
            Scan();   // its manifest read again
            std::string err;
            if (!LoadMod(e, &err)) { e->error = err; *reply = "mod " + id + ": " + err; return false; }
            e->error.clear();
        }
        *reply = ModJson(*e);
        return true;
    }
    auto it = g_commands.find(method);
    if (it == g_commands.end()) { *reply = "unknown method '" + method + "' (commands.list)"; return false; }
    Command c = it->second;
    MmlReply r;
    int rc = 0;
    std::string args = paramsJson.empty() ? "null" : paramsJson;
    if (DWORD ex = SafeCommand(c.fn, args.c_str(), &r, c.user, &rc)) {
        Faulted(c.mod, method.c_str(), ex);
        *reply = "the command faulted (exception " + std::to_string(ex) + "); its mod is unloaded at the next frame";
        return false;
    }
    if (r.error || rc != 0) { *reply = r.text.empty() ? "command failed (" + std::to_string(rc) + ")" : r.text; return false; }
    *reply = r.text.empty() ? "null" : r.text;
    return true;
}

std::string ModsList() {
    std::string out = "[";
    for (size_t i = 0; i < g_mods.size(); i++) out += (i ? "," : "") + ModJson(*g_mods[i]);
    return out + "]";
}

std::string CommandsList() {
    json out = json::array();
    for (const char *m : {"mods.list", "mods.rescan", "mod.reload", "mod.load", "mod.unload", "commands.list", "hooks.list",
                          "menu.state", "menu.command", "menu.value", "start.state", "input.actions", "input.act", "input.key",
                          "input.seq", "input.release", "input.state", "text.get", "text.find", "screenshot", "screenshot.state",
                          "draw.text", "draw.rect", "draw.line", "draw.clear", "draw.state", "config.get", "config.set",
                          "menu.item", "menu.items", "menu.item.remove", "menu.page", "menu.page.set", "menu.page.close", "menu.def", "menu.click", "input.mouse", "input.text"})
        out.push_back({{"name", m}, {"owner", "host"}});
    for (auto &kv : g_commands) out.push_back({{"name", kv.first}, {"owner", Who(kv.second.mod)}, {"help", kv.second.help}});
    return out.dump();
}

} // namespace host
