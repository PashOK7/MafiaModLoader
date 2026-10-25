// Mods: loading, the host table, events, commands, services, state (see host.h, mml.h, docs/MODDING.md).
//
// A mod lives in <game>\mml\mods\<id>\ with a mod.json manifest:
//   { "id": "core", "name": "...", "version": "0.1.0", "type": "native", "entry": "core.dll",
//     "enabled": true, "load_order": 0, "depends": [] }
// Each load of a native mod runs from a folder of its own, <game>\mml\live\<id>_<pid>_<n>\ (its DLL and copies of
// the other DLLs in its folder: lifecycle.cpp), so the originals can be rebuilt while the game runs; `mod.reload`
// unloads it (everything it registered goes with it) and loads the new copy. A mod that faults inside one of its
// callbacks is unloaded at the next frame and logged; the game goes on. A load whose code a thread still runs after
// its unload is not freed (that would crash the game) until the thread has left it.
#include "host.h"
#include <algorithm>
#include <cmath>
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
    std::string entry64;              // "entry64": its 64-bit half, run by the sidecar (sidecar.cpp); "" none
    bool enabled = true;
    int order = 100;
    std::vector<std::string> depends;
    std::vector<std::string> builds;  // the game's builds it is made for ("1.2", "1.0"); empty: every build
    bool gfx = false;                 // "gfx": true: it draws through the graphics bridge (the device made without multisampling)
    json settings;                    // "settings": its page in the game's Options > Mods (panel.cpp); null: none
    HMODULE dll = nullptr;
    MmlModUnloadFn unload = nullptr;
    MmlMod *current = nullptr;        // the live load, or null
    unsigned generation = 0;
    std::string liveDir;              // this load's folder (lifecycle.cpp)
    host::ProcessMeasure atLoad{};    // the process before this load
    json lastUnload;                  // what the last unload found (mods.list)
    bool faulted = false;            // a callback faulted: unloaded at the next frame
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
struct Kept { std::string id; unsigned generation; HMODULE dll; std::string liveDir; };
std::vector<Kept> g_kept;   // loads unloaded but not freed: a thread was still in their code

ModEntry *ById(const std::string &id) {
    for (auto &m : g_mods) if (m->id == id) return m.get();
    return nullptr;
}
bool Alive(const MmlMod *m) { return m && m->alive; }
const char *Who(const MmlMod *m) { return m ? m->entry->id.c_str() : "?"; }

// ---- calls into mods, each behind __try: a fault marks the mod for unloading instead of killing the game
thread_local uintptr_t t_faultAt, t_faultData;   // the last fault caught: where, and the address it touched
thread_local uintptr_t t_faultStack[16];          // and its call stack (crash.cpp StackOf)
thread_local int t_faultDepth;
int NoteFault(EXCEPTION_POINTERS *x) {
    t_faultAt = reinterpret_cast<uintptr_t>(x->ExceptionRecord->ExceptionAddress);
    t_faultData = x->ExceptionRecord->NumberParameters >= 2 ? x->ExceptionRecord->ExceptionInformation[1] : 0;
    t_faultDepth = StackOf(x->ContextRecord, t_faultStack, 16);
    return EXCEPTION_EXECUTE_HANDLER;
}
void Faulted(MmlMod *m, const char *where, DWORD code) {
    if (!m || m->entry->faulted) return;
    m->entry->faulted = true;
    char touched[48] = "";
    if (code == EXCEPTION_ACCESS_VIOLATION)
        snprintf(touched, sizeof touched, ", touching 0x%08X", static_cast<unsigned>(t_faultData));
    Log(MML_LOG_ERROR, "host", "mod %s faulted (exception %08X at %s%s) in %s: unloaded at the next frame", Who(m),
        static_cast<unsigned>(code), DescribeAddress(t_faultAt).c_str(), touched, where);
    char code8[16];
    snprintf(code8, sizeof code8, "%08X", static_cast<unsigned>(code));
    json stack = json::array();
    std::string line;
    for (int i = 0; i < t_faultDepth; i++) {
        std::string mod = ModAt(t_faultStack[i]);
        stack.push_back(DescribeAddress(t_faultStack[i]) + (mod.empty() ? "" : " (mod " + mod + ")"));
        line += (i ? " < " : "") + stack.back().get<std::string>();
    }
    if (!line.empty()) Log(MML_LOG_ERROR, "host", "  its stack: %s", line.c_str());
    json data = {{"name", Who(m)}, {"where", where}, {"exception", code8}, {"at", DescribeAddress(t_faultAt)}, {"stack", stack}};
    if (code == EXCEPTION_ACCESS_VIOLATION) data["touched"] = DescribeAddress(t_faultData);
    EventsRecord("MOD_FAULTED", data.dump(-1, ' ', false, json::error_handler_t::replace));
}
// Every call into a mod goes through one of these: behind the fault guard, with the x87 at 53 bits (fpu.cpp) and the
// game's word given back after, a fault too.
DWORD SafeEvent(MmlEventFn fn, const MmlEvent *ev, void *user) {
    uint16_t fpu = FpuDouble();
    DWORD r = 0;
    __try { fn(ev, user); } __except (NoteFault(GetExceptionInformation())) { r = GetExceptionCode(); }
    FpuRestore(fpu);
    return r;
}
DWORD SafeCommand(MmlCommandFn fn, const char *args, MmlReply *r, void *user, int *rc) {
    uint16_t fpu = FpuDouble();
    DWORD e = 0;
    __try { *rc = fn(args, r, user); } __except (NoteFault(GetExceptionInformation())) { e = GetExceptionCode(); }
    FpuRestore(fpu);
    return e;
}
DWORD SafeLoad(MmlModLoadFn fn, const MmlHost *h, MmlMod *m, int *rc) {
    uint16_t fpu = FpuDouble();
    DWORD r = 0;
    __try { *rc = fn(h, m); } __except (NoteFault(GetExceptionInformation())) { r = GetExceptionCode(); }
    FpuRestore(fpu);
    return r;
}
DWORD SafeUnload(MmlModUnloadFn fn, MmlMod *m) {
    uint16_t fpu = FpuDouble();
    DWORD r = 0;
    __try { fn(m); } __except (NoteFault(GetExceptionInformation())) { r = GetExceptionCode(); }
    FpuRestore(fpu);
    return r;
}
DWORD SafePosted(void (*fn)(void *), void *user) {
    uint16_t fpu = FpuDouble();
    DWORD r = 0;
    __try { fn(user); } __except (NoteFault(GetExceptionInformation())) { r = GetExceptionCode(); }
    FpuRestore(fpu);
    return r;
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
int H_HookMid(MmlMod *m, uintptr_t at, MmlMidFn fn, void *user) {
    if (!Alive(m)) return 0;
    std::string err;
    if (HookMid(m, at, fn, user, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "hook_mid %s failed: %s", DescribeAddress(at).c_str(), err.c_str());
    return 0;
}
int H_CodePatch(MmlMod *m, uintptr_t at, const void *expect, const void *bytes, size_t n) {
    if (!Alive(m)) return 0;
    if (!bytes) return PatchRemove(m, at) ? 1 : 0;
    std::string err;
    if (PatchAdd(m, at, expect, bytes, n, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "code_patch %s (%zu bytes) refused: %s", DescribeAddress(at).c_str(), n, err.c_str());
    return 0;
}
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
int H_ScriptRun(MmlMod *m, const char *code, void *owner, void *const *acts, int nacts, const float *flts, int nflts, uint32_t timeout,
                MmlScriptFn fn, void *user) {
    if (!Alive(m)) return 0;
    std::string err;
    int h = ScriptRun(m, code, owner, acts, nacts, flts, nflts, timeout, fn, user, &err);
    if (!h) Log(MML_LOG_WARN, Who(m), "script_run: %s", err.c_str());
    return h;
}
void H_ScriptStop(MmlMod *m, int handle) { if (Alive(m)) ScriptStop(m, handle); }
int H_ScriptState(MmlMod *m, int handle) { return Alive(m) ? ScriptState(m, handle) : -1; }
void *H_ActorSpawn(MmlMod *m, int kind, const char *model, const MmlVec3 *pos, const MmlVec3 *dir) {
    if (!Alive(m)) return nullptr;
    std::string err;
    void *a = WorldSpawn(m, kind, model, pos, dir, &err);
    if (!a) Log(MML_LOG_WARN, Who(m), "actor_spawn: %s", err.c_str());
    return a;
}
int H_ActorRemove(MmlMod *m, void *actor) {
    if (!Alive(m)) return 0;
    std::string err;
    if (WorldRemove(actor, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "actor_remove: %s", err.c_str());
    return 0;
}
int H_ActorTeleport(MmlMod *m, void *actor, const MmlVec3 *pos, const MmlVec3 *dir) {
    if (!Alive(m) || !pos) return 0;
    std::string err;
    if (WorldTeleport(actor, *pos, dir, true, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "actor_teleport: %s", err.c_str());
    return 0;
}
int H_ItemId(const char *name) {
    std::string err;
    return WorldItemId(name, &err);
}
int H_GiveItem(MmlMod *m, void *human, int item, int loaded, int reserve) {
    if (!Alive(m)) return 0;
    std::string err;
    if (WorldGive(human, item, loaded, reserve, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "give_item: %s", err.c_str());
    return 0;
}
int H_ActorSeat(MmlMod *m, void *human, void *car, int seat) {
    if (!Alive(m)) return 0;
    std::string err;
    if (WorldSeat(human, car, seat, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "actor_seat: %s", err.c_str());
    return 0;
}
int H_FileAdd(MmlMod *m, const char *name, const char *path) { return Alive(m) ? FilesAdd(m, name, path) : 0; }
int H_FileSource(const char *name, char *path, int size) { return FilesSource(name, path, size); }
int64_t H_FileRead(MmlMod *, const char *name, void *buf, size_t cap, int flags) { return FilesRead(name, buf, cap, flags); }
int H_GfxPass(MmlMod *m, int order, MmlGfxPassFn fn, void *user) {
    if (!Alive(m)) return 0;
    int h = GfxPassAdd(m, order, fn, user);
    if (!h) Log(MML_LOG_WARN, Who(m), "gfx_pass refused (no function, or too many passes)");
    return h;
}
void H_GfxPassRemove(MmlMod *m, int handle) { if (m && handle > 0) GfxPassRemove(m, handle); }
void H_GfxUi(MmlMod *m, int mode) { if (Alive(m)) GfxUiAsk(m, mode); }
int H_GfxPassViews(MmlMod *m, int pass, int on) { return Alive(m) && GfxPassViews(m, pass, on != 0) ? 1 : 0; }
int H_GfxMotion(MmlMod *m, int on) { return Alive(m) ? MotionAsk(m, on != 0) : 0; }
int H_SkinSet(MmlMod *m, const char *model, const char *mesh, const MmlSkinLod *lods, int count, char *error, int cap) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = SkinSet(m, model, mesh, lods, count, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "skin_set: %s", err.c_str());
    if (error && cap > 0) {
        size_t k = std::min<size_t>(err.size(), static_cast<size_t>(cap - 1));
        memcpy(error, err.data(), k);
        error[k] = 0;
    }
    return r;
}
int H_SkinGpu(MmlMod *m, int on) { return Alive(m) ? SkinGpuAsk(m, on != 0) : 0; }
int H_ShaderConstants(MmlMod *m, int handle, int reg, const float *values, int count, char *error, int cap) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = ShaderConstants(m, handle, reg, values, count, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "shader_constants: %s", err.c_str());
    if (error && cap > 0) {
        size_t k = std::min<size_t>(err.size(), static_cast<size_t>(cap - 1));
        memcpy(error, err.data(), k);
        error[k] = 0;
    }
    return r;
}
int H_ShaderVertexData(MmlMod *m, const char *visual, int lod, const float *attr, int vertices) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = VdataSet(m, visual, lod, attr, vertices, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "shader_vertex_data: %s", err.c_str());
    return r;
}
int H_SkinGet(MmlMod *m, const char *model, const char *mesh, int lod, MmlSkinMesh *out) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = SkinGet(m, model, mesh, lod, out, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "skin_get: %s", err.c_str());
    return r;
}
int H_GfxState(MmlMod *, char *why, int cap) {
    std::string w;
    int s = GfxStateNow(&w);
    if (why && cap > 0) {
        size_t n = std::min<size_t>(w.size(), static_cast<size_t>(cap - 1));
        while (n > 0 && n < w.size() && (static_cast<unsigned char>(w[n]) & 0xC0) == 0x80) n--;   // (whole UTF-8 characters)
        memcpy(why, w.data(), n);
        why[n] = 0;
    }
    return s;
}
int H_GfxShowStats(MmlMod *, MmlGfxShowStats *out) {
    if (!out || out->size < sizeof(MmlGfxShowStats)) return 0;
    GfxShowStats(out);
    return 1;
}
int H_SceneView(MmlMod *m, int w, int h, MmlViewFn fn, void *user) {
    if (!Alive(m)) return 0;
    std::string err;
    int handle = ViewAdd(m, w, h, fn, user, &err);
    if (!handle) Log(MML_LOG_WARN, Who(m), "scene_view refused: %s", err.c_str());
    return handle;
}
void H_SceneViewRemove(MmlMod *m, int handle) { if (m && handle > 0) ViewRemove(m, handle); }
int H_GeomHide(MmlMod *m, const char *pattern, int where) {
    if (!Alive(m)) return 0;
    std::string err;
    int h = GeomHide(m, pattern, where, &err);
    if (!h) Log(MML_LOG_WARN, Who(m), "geom_hide refused: %s", err.c_str());
    return h;
}
void H_GeomShow(MmlMod *m, int handle) { if (m && handle >= 0) GeomShow(m, handle); }
int H_FrameStats(MmlMod *m, MmlFrameStats *out) { return Alive(m) && ProfFrameStats(m, out) ? 1 : 0; }
int H_HumansList(MmlMod *m, void **out, int max) { return Alive(m) ? HumansList(out, max) : 0; }
int H_HumanSkeleton(MmlMod *m, void *human, MmlBone *out, int max) {
    return Alive(m) && out && max >= MML_BONE_COUNT ? HumanSkeleton(human, out, max) : 0;
}
int H_BoneSet(MmlMod *m, void *frame, const float *rot, const float *pos) { return Alive(m) ? BoneSet(frame, rot, pos) : 0; }
void *H_HumanPlayer(MmlMod *m) { return Alive(m) ? HumanPlayer() : nullptr; }
int H_CameraOverride(MmlMod *m, MmlViewFn fn, void *user) { return Alive(m) ? CameraOverride(m, fn, user) : 0; }
int H_CameraProject(MmlMod *m, const float w[3], float s[3]) { return Alive(m) && w && s ? RenderProject(w, s) : 0; }
int H_CameraRay(MmlMod *m, float x, float y, float o[3], float d[3]) { return Alive(m) && o && d ? RenderRay(x, y, o, d) : 0; }
int H_WorldRay(MmlMod *m, const float f[3], const float t[3], uint32_t what, void *ignore, MmlHit *hit) {
    return Alive(m) && f && t && hit ? WorldRay(f, t, what, ignore, hit) : 0;
}
int H_WorldSweep(MmlMod *m, const float f[3], const float t[3], float r, uint32_t what, void *ignore, MmlHit *hit) {
    return Alive(m) && f && t && hit ? WorldSweep(f, t, r, what, ignore, hit) : 0;
}
int H_WorldNear(MmlMod *m, const float at[3], float radius, int type, void **out, int max) {
    return Alive(m) && at ? WorldNear(at, radius, type, out, out ? max : 0) : 0;
}
int H_Money(MmlMod *m) { return Alive(m) ? StateMoney() : INT32_MIN; }
int H_MoneySet(MmlMod *m, int v) {
    if (!Alive(m)) return 0;
    std::string err;
    if (StateMoneySet(v, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "money_set: %s", err.c_str());
    return 0;
}
float H_HumanHealth(MmlMod *m, void *h, float *max) { return Alive(m) ? StateHealth(h, max) : -1.0f; }
int H_HumanHealthSet(MmlMod *m, void *h, float v) {
    if (!Alive(m)) return 0;
    std::string err;
    if (StateHealthSet(h, v, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "human_health_set: %s", err.c_str());
    return 0;
}
int H_HumanItems(MmlMod *m, void *h, MmlItem *out, int max) { return Alive(m) ? StateItems(h, out, out ? max : 0) : 0; }
int H_ItemAmmoSet(MmlMod *m, void *h, int slot, int loaded, int reserve) {
    if (!Alive(m)) return 0;
    std::string err;
    if (StateAmmoSet(h, slot, loaded, reserve, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "item_ammo_set: %s", err.c_str());
    return 0;
}
int H_HumanTakeItem(MmlMod *m, void *h, int item) {
    if (!Alive(m)) return 0;
    std::string err;
    if (StateTakeItem(h, item, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "human_take_item: %s", err.c_str());
    return 0;
}
int H_SceneLights(MmlMod *m, MmlLight *out, int max) { return Alive(m) ? SceneLights(out, out ? max : 0) : 0; }
int H_LightSet(MmlMod *m, void *light, const float color[3], float power, float rn, float rf, int on) {
    if (!Alive(m)) return 0;
    std::string err;
    if (LightSet(light, color, power, rn, rf, on, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "light_set: %s", err.c_str());
    return 0;
}
int H_SceneSun(MmlMod *m, float s[3], float c[3], float a[3]) { return Alive(m) ? SceneSun(s, c, a) : 0; }
int H_CarExhaust(MmlMod *m, void *car, MmlExhaust *out) { return Alive(m) ? CarExhaust(car, out) : 0; }
int H_ExhaustHook(MmlMod *m, MmlExhaustFn fn, void *user) { return Alive(m) ? ExhaustHook(m, fn, user) : 0; }
int H_ParticleDef(MmlMod *m, int index, MmlParticleDef *out) { return Alive(m) ? ParticleDef(index, out) : 0; }
int H_ParticleDefSet(MmlMod *m, int index, const MmlParticleDef *d) { return Alive(m) ? ParticleDefSet(index, d) : 0; }
int H_ParticleHook(MmlMod *m, MmlParticleFn fn, void *user) { return Alive(m) ? ParticleHook(m, fn, user) : 0; }
int H_WeatherGet(MmlMod *m, int param, double *v) { return Alive(m) ? WeatherGet(param, v) : 0; }
int H_WeatherSet(MmlMod *m, int param, double v) { return Alive(m) ? WeatherSet(param, v) : 0; }
int H_WeatherReset(MmlMod *m) { return Alive(m) ? WeatherReset() : 0; }
void *H_PropCreate(MmlMod *m, const char *model, void *parent, const float pos[3], const float rot[4], float scale) {
    if (!Alive(m)) return nullptr;
    std::string err;
    void *p = PropCreate(m, model, parent, pos, rot, scale, &err);
    if (!p) Log(MML_LOG_WARN, Who(m), "prop_create: %s", err.c_str());
    return p;
}
int H_PropPlace(MmlMod *m, void *prop, const float pos[3], const float rot[4], float scale) {
    std::string err;
    return Alive(m) && PropPlace(prop, pos, rot, scale, &err) ? 1 : 0;
}
int H_PropAttach(MmlMod *m, void *prop, void *parent, int keep) {
    if (!Alive(m)) return 0;
    std::string err;
    if (PropAttach(prop, parent, keep != 0, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "prop_attach: %s", err.c_str());
    return 0;
}
int H_PropShow(MmlMod *m, void *prop, int on) { std::string err; return Alive(m) && PropShow(prop, on != 0, &err) ? 1 : 0; }
int H_PropRemove(MmlMod *m, void *prop) { std::string err; return Alive(m) && PropRemove(prop, &err) ? 1 : 0; }
int H_PropAnimPlay(MmlMod *m, void *prop, const char *anim, uint32_t flags, float weight) {
    if (!Alive(m)) return -1;
    std::string err;
    int n = PropAnimPlay(prop, anim, flags, weight, &err);
    if (n <= 0) Log(MML_LOG_WARN, Who(m), "prop_anim_play: %s", err.c_str());
    return n;
}
int H_PropAnimStop(MmlMod *m, void *prop) { std::string err; return Alive(m) && PropAnimStop(prop, &err) ? 1 : 0; }
int H_PropAnimState(MmlMod *m, void *prop, MmlPropAnim *out) { return Alive(m) ? PropAnimState(prop, out) : 0; }
int H_PropPhysics(MmlMod *m, void *prop, const MmlPropPhysics *p) {
    if (!Alive(m)) return 0;
    std::string err;
    if (PropPhysics(prop, p, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "prop_physics: %s", err.c_str());
    return 0;
}
int H_PropPush(MmlMod *m, void *prop, const float impulse[3], const float at[3]) {
    std::string err;
    return Alive(m) && impulse && PropPush(prop, impulse, at, &err) ? 1 : 0;
}
void *H_PropBody(MmlMod *m, void *prop) { return Alive(m) ? PropBody(prop) : nullptr; }
int H_GameFiles(MmlMod *m, const char *pattern, MmlFileFn fn, void *user) { return Alive(m) ? GameFiles(pattern, fn, user) : 0; }
int H_ModelFile(MmlMod *m, void *frame, char *out, int cap) {
    if (!Alive(m) || !frame) return 0;
    std::string f;
    int n = ModelFile(frame, &f);
    if (out && cap > 0) {
        size_t k = std::min<size_t>(f.size(), static_cast<size_t>(cap - 1));
        memcpy(out, f.data(), k);
        out[k] = 0;
    }
    return n;
}
int H_ModelOpen(MmlMod *m, const char *file) {
    if (!Alive(m)) return 0;
    std::string err;
    int id = ModelOpen(m, file, &err);
    if (!id) Log(MML_LOG_WARN, Who(m), "model_open: %s", err.c_str());
    return id;
}
int H_ModelNodes(MmlMod *m, int model, MmlModelNode *out, int max) { return Alive(m) ? ModelNodes(model, out, max) : 0; }
int H_ModelMesh(MmlMod *m, int model, int node, int lod, MmlModelMesh *out) { return Alive(m) ? ModelMesh(model, node, lod, out) : 0; }
int H_ModelMaterials(MmlMod *m, int model, MmlModelMaterial *out, int max) { return Alive(m) ? ModelMaterials(model, out, max) : 0; }
void H_ModelClose(MmlMod *m, int model) { if (Alive(m)) ModelClose(m, model); }
int H_SceneFocus(MmlMod *m, const float lo[3], const float hi[3]) { return Alive(m) && SceneFocus(m, lo && hi ? lo : nullptr, hi) ? 1 : 0; }
int H_ScenePieces(MmlMod *m, MmlMeshPiece *out, int max) { return Alive(m) ? ScenePieces(m, out, max) : 0; }
int H_ActorMeshes(MmlMod *m, void *actor, MmlMeshPiece *out, int max) { return Alive(m) && actor ? ActorMeshes(actor, out, max) : -1; }
int H_ShaderAdd(MmlMod *m, const MmlShaderRule *rule, char *error, int cap) {
    if (!Alive(m)) return 0;
    std::string err;
    int h = ShaderAdd(m, rule, &err);
    if (!h) Log(MML_LOG_WARN, Who(m), "shader_add: %s", err.c_str());
    if (error && cap > 0) {
        size_t k = std::min<size_t>(err.size(), static_cast<size_t>(cap - 1));
        memcpy(error, err.data(), k);
        error[k] = 0;
    }
    return h;
}
int H_ShaderRemove(MmlMod *m, int handle) { return Alive(m) && ShaderRemove(m, handle) ? 1 : 0; }
int H_ShaderShadows(MmlMod *m, const MmlShadowSettings *s) { return Alive(m) && ShaderShadows(m, s) ? 1 : 0; }
int H_GameCamera(MmlMod *m, MmlGameCamera *out) { return Alive(m) ? GameCamera(out) : 0; }
int H_GameCameraMode(MmlMod *m, int mode) { return Alive(m) ? GameCameraMode(m, mode) : 0; }
int H_GameCameraFov(MmlMod *m, float fov) { return Alive(m) ? GameCameraFov(m, fov) : 0; }
void *H_ActorFrame(MmlMod *m, void *actor) { return Alive(m) ? ActorFrame(actor) : nullptr; }
int H_FrameWorld(MmlMod *m, void *frame, float out[16]) { return Alive(m) && out ? FrameWorld(frame, out) : 0; }
int H_SoundPlay(MmlMod *m, const char *file, void *frame, const float pos[3], float volume, float pitch, float rangeNear, float rangeFar, uint32_t flags) {
    if (!Alive(m)) return 0;
    std::string err;
    int id = SoundPlay(m, file, frame, pos, volume, pitch, rangeNear, rangeFar, flags, &err);
    if (!id) Log(MML_LOG_WARN, Who(m), "sound_play: %s", err.c_str());
    return id;
}
int H_SoundSet(MmlMod *m, int sound, float volume, float pitch) { std::string err; return Alive(m) && SoundSet(sound, volume, pitch, &err) ? 1 : 0; }
int H_SoundMove(MmlMod *m, int sound, void *frame, const float pos[3]) { std::string err; return Alive(m) && SoundMove(sound, frame, pos, &err) ? 1 : 0; }
int H_SoundStop(MmlMod *m, int sound) { std::string err; return Alive(m) && SoundStop(sound, &err) ? 1 : 0; }
int H_SoundState(MmlMod *m, int sound, MmlSoundState *out) { return Alive(m) ? SoundState(sound, out) : 0; }
int H_MusicPlay(MmlMod *m, const char *file, float volume, uint32_t flags) {
    if (!Alive(m)) return 0;
    std::string err;
    int id = MusicPlay(m, file, volume, flags, &err);
    if (!id) Log(MML_LOG_WARN, Who(m), "music_play: %s", err.c_str());
    return id;
}
int H_MusicSet(MmlMod *m, int music, float volume) { std::string err; return Alive(m) && MusicSet(music, volume, &err) ? 1 : 0; }
int H_MusicStop(MmlMod *m, int music) { std::string err; return Alive(m) && MusicStop(music, &err) ? 1 : 0; }
int H_MusicGame(MmlMod *m, int on) { return Alive(m) ? MusicGame(m, on) : -1; }
int H_KeyCapture(MmlMod *m, int key, int on) { return Alive(m) && KeyCapture(m, key, on != 0) ? 1 : 0; }
int H_ActionCapture(MmlMod *m, int slot, int on) { return Alive(m) && ActionCapture(m, slot, on != 0) ? 1 : 0; }
int H_PlayerLock(MmlMod *m, int on) { return Alive(m) ? PlayerLock(m, on) : 0; }
int H_VehicleHook(MmlMod *m, MmlVehicleFn fn, void *user) { return Alive(m) ? VehicleHook(m, fn, user) : 0; }
int H_VehicleBody(MmlMod *m, void *car, MmlVehicleBody *read, const MmlVehicleBody *write) { return Alive(m) ? VehicleBody(car, read, write) : 0; }
int H_VehicleKinds(MmlMod *m, unsigned kinds) { return Alive(m) ? VehicleKinds(m, kinds) : 0; }
int H_HumanOrder(MmlMod *m, void *human, const char *code, void *const *acts, int nacts, const float *flts, int nflts, const float *points,
                 int npoints) {
    if (!Alive(m)) return 0;
    std::string err;
    int id = HumanOrder(m, human, code, acts, nacts, flts, nflts, points, npoints, &err);
    if (!id) Log(MML_LOG_WARN, ModIdOf(m), "human_order: %s", err.c_str());
    return id;
}
int H_HumanOrderState(MmlMod *m, void *human, MmlOrderState *out) { return Alive(m) ? HumanOrderState(human, out) : 0; }
int H_RoadNear(MmlMod *m, const float at[3], float out[3]) {
    if (!Alive(m) || !at) return 0;
    float p[3];
    int id = RoadNear(at, p);
    if (id < 0) return 0;
    if (out) for (int i = 0; i < 3; i++) out[i] = p[i];
    return id + 1;
}
int H_HumanInvulnerable(MmlMod *m, void *human, int on) {
    if (!Alive(m)) return -1;
    std::string err;
    int r = StateInvulnerableSet(m, human, on < 0 ? -1 : on ? 1 : 0, &err);
    if (r < 0) Log(MML_LOG_WARN, ModIdOf(m), "human_invulnerable: %s", err.c_str());
    return r;
}
int H_HudGame(MmlMod *m, int show) {
    if (!Alive(m)) return -1;
    std::string err;
    int r = HudGame(m, show < 0 ? -1 : show ? 1 : 0, &err);
    if (r < 0) Log(MML_LOG_WARN, ModIdOf(m), "hud_game: %s", err.c_str());
    return r;
}
int H_WebNodes(MmlMod *m, const float at[3], float radius, MmlWebNode *out, int max) {
    return Alive(m) && at ? WebNodes(at, radius, out, out ? max : 0) : 0;
}
const char *H_MaterialName(int material) { return MaterialName(material); }
MmlChannel *H_ChannelOpen(MmlMod *m, const char *name, uint32_t size) {
    if (!Alive(m) || !name || !*name) return nullptr;
    std::string err;
    MmlChannel *c = SidecarChannelOpen(m, name, size, &err);
    if (!c) Log(MML_LOG_WARN, ModIdOf(m), "channel_open %s: %s", name, err.c_str());
    return c;
}
int H_ChannelWrite(MmlChannel *ch, const void *data, uint32_t bytes) { return SidecarChannelWrite(ch, data, bytes); }
uint32_t H_ChannelRead(MmlChannel *ch, void *data, uint32_t cap, uint64_t *version) { return SidecarChannelRead(ch, data, cap, version); }
uint64_t H_ChannelWait(MmlChannel *ch, uint64_t after, uint32_t timeoutMs) { return SidecarChannelWait(ch, after, timeoutMs); }
int H_GfxSharedTexture(MmlMod *m, const char *name, MmlSharedTexture *out) { return Alive(m) ? GfxSharedTexture(m, name, out) : 0; }
int H_GfxSharedFence(MmlMod *m, const char *name, MmlSharedFence *out) { return Alive(m) ? GfxSharedFence(m, name, out) : 0; }
int H_GfxShow(MmlMod *m, MmlGfxShowFn fn, void *user) { return Alive(m) ? GfxShowSet(m, fn, user) : 0; }
int H_GfxJitter(MmlMod *m, int on, float x, float y) { return Alive(m) ? GfxJitterSet(m, on, x, y) : 0; }
int H_GfxUpscale(MmlMod *m, unsigned w, unsigned h, MmlGfxUpscaleFn fn, void *user) { return Alive(m) ? GfxUpscaleSet(m, w, h, fn, user) : 0; }
int H_ZoneAdd(MmlMod *m, const float pos[3], float radius, void *frame, MmlZoneFn fn, void *user) {
    std::string err;
    int id = Alive(m) && fn ? ZoneAdd(m, pos, radius, frame, fn, user, &err) : 0;
    if (Alive(m) && !id) Log(MML_LOG_WARN, Who(m), "zone_add: %s", fn ? err.c_str() : "no function");
    return id;
}
int H_ZoneWatch(MmlMod *m, int zone, uint32_t who, void *actor) {
    if (!Alive(m)) return 0;
    std::string err;
    if (ZoneWatch(m, zone, who, actor, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "zone_watch: %s", err.c_str());
    return 0;
}
int H_WorldWater(MmlMod *m, const float at[3], MmlWater *out) { return Alive(m) && at && out ? WorldWater(at, out) : 0; }
int H_UseAdd(MmlMod *m, const float pos[3], float radius, void *frame, const char *label, MmlUseFn fn, void *user) {
    std::string err;
    int id = Alive(m) && fn ? UseAdd(m, pos, radius, frame, label, fn, user, &err) : 0;
    if (Alive(m) && !id) Log(MML_LOG_WARN, Who(m), "use_point_add: %s", fn ? err.c_str() : "no function");
    return id;
}
int H_TimerAdd(MmlMod *m, int ms, int repeat, MmlTimerFn fn, void *user) {
    std::string err;
    return Alive(m) && fn ? TimerAdd(m, ms, repeat != 0, fn, user, &err) : 0;
}
int H_InteractRemove(MmlMod *m, int id) { return Alive(m) && InteractRemove(m, id) ? 1 : 0; }
int H_PictureLoad(MmlMod *m, const char *file, const char *alpha) {
    if (!Alive(m)) return 0;
    std::string err;
    int id = HudPictureLoad(m, file, alpha, &err);
    if (!id) Log(MML_LOG_WARN, Who(m), "picture_load: %s", err.c_str());
    return id;
}
int H_PictureSize(int picture, int *w, int *h) { return HudPictureSize(picture, w, h) ? 1 : 0; }
void H_PictureFree(MmlMod *m, int picture) { if (Alive(m)) HudPictureFree(m, picture); }
void H_DrawPicture(MmlMod *m, int picture, float x, float y, float w, float h, float angle, uint32_t argb, const float uv[4]) {
    if (Alive(m)) DrawPicture(m, picture, x, y, w, h, angle, argb, uv);
}
int H_HudConsole(MmlMod *m, const char *text, uint32_t argb) {
    if (!Alive(m)) return 0;
    std::string err;
    if (HudConsole(text ? text : "", argb, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "hud_console: %s", err.c_str());
    return 0;
}
int H_HudHelp(MmlMod *m, const char *text) { return Alive(m) && text && *text ? HudHelpLine(text) : 0; }
int H_HudMarkerAdd(MmlMod *m, const MmlHudMarker *marker) {
    if (!Alive(m)) return 0;
    std::string err;
    int id = HudMarkerAdd(m, marker, &err);
    if (!id) Log(MML_LOG_WARN, Who(m), "hud_marker_add: %s", err.c_str());
    return id;
}
int H_HudMarkerSet(MmlMod *m, int id, const MmlHudMarker *marker) {
    if (!Alive(m)) return 0;
    std::string err;
    if (HudMarkerSet(m, id, marker, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "hud_marker_set: %s", err.c_str());
    return 0;
}
int H_HudMarkerRemove(MmlMod *m, int id) { return Alive(m) && HudMarkerRemove(m, id) ? 1 : 0; }
int H_SavePut(MmlMod *m, int scope, const char *key, const void *data, size_t n) {
    if (!Alive(m)) return 0;
    std::string err;
    if (SavePut(m, scope, key, data, n, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "save_put: %s", err.c_str());
    return 0;
}
size_t H_SaveGet(MmlMod *m, int scope, const char *key, void *data, size_t cap) { return Alive(m) ? SaveGet(m, scope, key, data, cap) : 0; }
int H_HumanAnimPlay(MmlMod *m, void *human, const char *anim, int flags) {
    if (!Alive(m)) return -1;
    std::string err;
    int ms = HumanAnimPlay(human, anim, flags, &err);
    if (ms < 0) Log(MML_LOG_WARN, Who(m), "human_anim_play: %s", err.c_str());
    return ms;
}
int H_HumanAnimStop(MmlMod *m, void *human) { std::string err; return Alive(m) && HumanAnimStop(human, &err) ? 1 : 0; }
int H_HumanAnimReplace(MmlMod *m, void *human, const char *own, const char *with) {
    if (!Alive(m)) return 0;
    std::string err;
    if (HumanAnimReplace(m, human, own, with, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "human_anim_replace: %s", err.c_str());
    return 0;
}
int H_HumanAnimState(MmlMod *m, void *human, MmlHumanAnim *out) { return Alive(m) ? HumanAnimState(human, out) : 0; }
int H_ItemDefine(MmlMod *m, const MmlItemDef *def) {
    if (!Alive(m) || !def) return 0;
    if (def->size < offsetof(MmlItemDef, collected)) { Log(MML_LOG_WARN, Who(m), "item_define: an MmlItemDef too small"); return 0; }
    MmlItemDef d{};
    std::memcpy(&d, def, std::min<size_t>(def->size, sizeof d));
    std::string err;
    int id = ItemDefine(ModIdOf(m), d, &err);
    if (!id) Log(MML_LOG_WARN, Who(m), "item_define %s: %s", d.name ? d.name : "?", err.c_str());
    return id;
}
int H_ItemFind(MmlMod *m, const char *name) { return Alive(m) && name ? ItemFind(ModIdOf(m), name) : 0; }
int H_StatsGet(MmlMod *m, int scope, MmlStats *out) { return Alive(m) ? StatsGet(scope, out) : 0; }
int H_ProfileProgress(MmlMod *m, MmlProgress *out) { return Alive(m) ? ProfileProgress(out) : 0; }
int H_ProfileSaves(MmlMod *m, int *slots, int max) { return Alive(m) ? ProfileSaves(slots, slots ? max : 0) : 0; }
int H_HumanGetUp(MmlMod *m, void *human, int blend_ms) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = HumanGetUp(human, blend_ms, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "human_get_up: %s", err.c_str());
    return r ? 1 : 0;
}
int H_HitFilter(MmlMod *m, MmlHitFilterFn fn, void *user) { return Alive(m) ? HitFilterSet(m, fn, user) : 0; }
int H_HumanHit(MmlMod *m, void *person, const MmlHumanHit *hit) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = HumanHitCall(person, hit, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "human_hit: %s", err.c_str());
    return r;
}
int H_BulletShape(MmlMod *m, void *person, const MmlCapsule *parts, int count) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = BulletShapeSet(m, person, parts, count, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "bullet_shape: %s", err.c_str());
    return r;
}
int H_FrameWatch(MmlMod *m, void *frame, MmlFrameGoneFn fn, void *user) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = FrameWatch(m, frame, fn, user, false, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "frame_watch: %s", err.c_str());
    return r;
}
int H_ActorScale(MmlMod *m, void *actor, const MmlActorScale *s) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = ActorScale(m, actor, s, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "actor_scale: %s", err.c_str());
    return r;
}
int H_ScaleBeat(MmlMod *m) {
    if (!Alive(m)) return 0;
    ScaleBeat();
    return 1;
}
int H_SceneReady(MmlMod *m, const float lo[3], const float hi[3]) { return Alive(m) && SceneReady(m, lo, hi) ? 1 : 0; }
int H_DrawWorld(MmlMod *m, const MmlDrawShape *shape) {
    if (!Alive(m)) return 0;
    std::string err;
    int r = DrawWorldAdd(m, shape, &err);
    if (!r) Log(MML_LOG_WARN, Who(m), "draw_world: %s", err.c_str());
    return r;
}
int H_PedestriansList(MmlMod *m, MmlPedestrian *out, int max) {
    if (!Alive(m)) return 0;
    std::vector<MmlPedestrian> v;
    for (void *e : PedestrianElements()) {
        MmlPedestrian p{};
        if (PedestrianRead(e, &p)) v.push_back(p);
    }
    for (int i = 0; out && i < max && i < static_cast<int>(v.size()); i++) out[i] = v[i];
    return static_cast<int>(v.size());
}
int H_WorldShapes(MmlMod *m, const float lo[3], const float hi[3], uint32_t what, MmlShape *out, int max) {
    return Alive(m) && lo && hi ? WorldShapes(lo, hi, what, out, out ? max : 0) : 0;
}
int H_GeomCapture(MmlMod *m, const char *pattern, MmlGeomFn fn, void *user) {
    if (!Alive(m)) return 0;
    std::string err;
    if (GeomCapture(m, pattern, fn, user, &err)) return 1;
    Log(MML_LOG_WARN, Who(m), "geom_capture refused: %s", err.c_str());
    return 0;
}

// ---- manifests
bool ReadManifest(const std::string &dir, const std::string &folder, ModEntry *e, std::string *err) {
    std::ifstream f(Wide(dir + "mod.json"));
    if (!f) { *err = "no mod.json"; return false; }
    try {
        json j = json::parse(f, nullptr, true, true);   // comments allowed
        e->id = j.value("id", folder);
        e->name = j.value("name", e->id);
        e->version = j.value("version", "0.0.0");
        e->type = j.value("type", "native");
        e->entry64 = j.value("entry64", std::string());
        // a 64-bit half alone: "entry64" and no "entry"
        e->entry = j.contains("entry") || e->entry64.empty() ? j.value("entry", e->id + ".dll") : std::string();
        e->enabled = j.value("enabled", true);
        e->order = j.value("load_order", 100);
        e->depends.clear();
        if (j.contains("depends") && j["depends"].is_array())
            for (auto &d : j["depends"]) if (d.is_string()) e->depends.push_back(d.get<std::string>());
        e->gfx = j.value("gfx", false);
        e->settings = j.contains("settings") && j["settings"].is_object() ? j["settings"] : json();
        e->builds.clear();
        if (j.contains("builds") && j["builds"].is_array())
            for (auto &b : j["builds"]) if (b.is_string()) e->builds.push_back(b.get<std::string>());
        e->dir = dir;
        return true;
    } catch (const std::exception &ex) {
        *err = std::string("mod.json: ") + ex.what();
        return false;
    }
}

bool UnloadMod(ModEntry *e, const char *why);

// Made for the running build: its "builds" name it, or it names none. With no known game (the stand-in game of
// the tests) every mod is.
bool ForThisBuild(const ModEntry &e) {
    std::string build = GameBuild();
    if (e.builds.empty() || build == "unknown") return true;
    return std::find(e.builds.begin(), e.builds.end(), build) != e.builds.end();
}
std::string BuildsText(const ModEntry &e) {
    std::string s;
    for (const std::string &b : e.builds) s += (s.empty() ? "" : ", ") + b;
    return s;
}

// A mod of files alone ("type": "files"): no DLL, its files\ folder is the whole mod (files.cpp).
bool LoadFilesOnly(ModEntry *e) {
    MmlMod *m = new MmlMod{e, true};
    e->dll = nullptr;
    e->unload = nullptr;
    e->current = m;
    e->liveDir.clear();
    e->faulted = false;
    ++e->generation;
    Log(MML_LOG_INFO, "host", "mod %s %s loaded (%s: %zu files, generation %u)", e->id.c_str(), e->version.c_str(),
        e->type == "files" ? "files only" : "no 32-bit DLL, its 64-bit half in the sidecar", FilesCount(e->id), e->generation);
    MmlEvent ev{};
    ev.size = sizeof ev;
    ev.type = MML_EVENT_MOD_LOADED;
    ev.name = e->id.c_str();
    Dispatch(ev);
    return true;
}

bool LoadModDll(ModEntry *e, std::string *err);

// A mod's load: its files in the game first (its mml_mod_load may read them), then its DLL. keepFiles: a failed load
// leaves its files in (LoadEnabled's passes: a mod whose dependency loads in a later pass; it takes out what failed
// at the end).
bool LoadMod(ModEntry *e, std::string *err, bool keepFiles = false) {
    if (e->current) { *err = "already loaded"; return false; }
    bool ok = true;
    for (const std::string &d : e->depends) {
        ModEntry *dep = ById(d);
        if (!dep || !dep->current) { *err = "depends on " + d + ", which is not loaded"; ok = false; break; }
    }
    if (ok && e->type != "native" && e->type != "files") { *err = "type " + e->type + " has no runtime yet"; ok = false; }
    if (ok && !ForThisBuild(*e)) {
        *err = "made for the game's build " + BuildsText(*e) + ", not " + GameBuild() + " (\"builds\" in its mod.json)";
        ok = false;
    }
    if (ok) {
        FilesModOn(e->id, e->dir, e->order, false);
        ok = e->type == "files" || e->entry.empty() ? LoadFilesOnly(e) : LoadModDll(e, err);
    }
    if (!ok && !keepFiles) FilesModOff(e->id);
    if (ok && !e->entry64.empty())   // its 64-bit half, with the 32-bit one (or alone): the sidecar loads it
        SidecarModLoad(e->id, e->dir, e->entry64, ModDataDir(e->current), e->generation);
    return ok;
}

bool LoadModDll(ModEntry *e, std::string *err) {
    char tag[128];
    snprintf(tag, sizeof tag, "%s_%u_%u", e->id.c_str(), static_cast<unsigned>(GetCurrentProcessId()), ++e->generation);
    std::string liveDir = MmlDir() + "live\\" + tag + "\\";
    std::string path = liveDir + "mod_" + tag + ".dll";
    e->atLoad = MeasureProcess();
    std::vector<std::string> libs;
    if (!LiveCopy(e->dir, e->entry, liveDir, path, &libs, err)) { LiveRemove(liveDir); return false; }
    DllDirScope dllDir(liveDir);   // (a library loaded by a bare name in mml_mod_load is found in the load's folder)
    HMODULE dll = PathLoadLibrary(path, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        DWORD code = GetLastError();
        *err = "LoadLibrary failed (" + std::to_string(code) + ")";
        if (code == ERROR_MOD_NOT_FOUND) *err += ": a DLL it imports is neither in its folder nor the game's nor the system's";
        LiveRemove(liveDir);
        return false;
    }
    auto load = reinterpret_cast<MmlModLoadFn>(GetProcAddress(dll, "mml_mod_load"));
    auto unload = reinterpret_cast<MmlModUnloadFn>(GetProcAddress(dll, "mml_mod_unload"));
    auto api = reinterpret_cast<MmlModApiFn>(GetProcAddress(dll, "mml_mod_api"));
    uint32_t want = api ? api() : MML_API_VERSION;
    if (!load || want > MML_API_VERSION) {
        FreeLibrary(dll);
        LiveRemove(liveDir);
        *err = !load ? "no mml_mod_load export" : "made for a newer loader (API " + std::to_string(want) + ")";
        return false;
    }
    MmlMod *m = new MmlMod{e, true};
    e->dll = dll;
    e->unload = unload;
    e->current = m;
    e->liveDir = liveDir;
    e->faulted = false;
    int rc = 0;
    DWORD ex;
    {
        ProfScope ps(m, "mml_mod_load");
        ex = SafeLoad(load, &g_host, m, &rc);
    }
    if (ex || rc != 0) {
        *err = ex ? "mml_mod_load faulted at " + DescribeAddress(t_faultAt) : "mml_mod_load returned " + std::to_string(rc);
        e->unload = nullptr;   // (a failed load is not unloaded by the mod; what it registered goes all the same)
        UnloadMod(e, "its load failed");
        return false;
    }
    for (const std::string &s : LibsElsewhere(liveDir, libs))
        Log(MML_LOG_WARN, "host", "mod %s: its library %s, not its own copy (a load of it not freed, or another mod's "
            "or the game's of the same name)", e->id.c_str(), s.c_str());
    Log(MML_LOG_INFO, "host", "mod %s %s loaded (generation %u%s)", e->id.c_str(), e->version.c_str(), e->generation,
        libs.empty() ? "" : (", " + std::to_string(libs.size()) + " libraries beside it").c_str());
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
    if (!e->entry64.empty()) SidecarModUnload(e->id, why);
    if (e->unload && !e->faulted) {
        DWORD ex;
        {
            ProfScope ps(m, "mml_mod_unload");
            ex = SafeUnload(e->unload, m);
        }
        if (ex) Log(MML_LOG_ERROR, "host", "mod %s faulted in mml_mod_unload (%08X at %s)", e->id.c_str(), static_cast<unsigned>(ex), DescribeAddress(t_faultAt).c_str());
    }
    m->alive = false;
    HookRemoveAll(m);
    PatchRemoveAll(m);   // (after its hooks: the bytes under a patch are the game's again before anything else runs)
    HotkeysRemove(m);
    MenusRemove(m);
    ScriptsRemove(m);
    DrawRemove(m);
    GfxRemove(m);
    ViewRemove(m, 0);
    GeomRemove(m);
    EffectsModGone(m);
    PropsModGone(m);
    ModelsModGone(m);
    MeshesModGone(m);
    ShadersModGone(m);
    SkinModGone(m);
    ShadowsModGone(m);
    GameCameraModGone(m);
    SoundModGone(m);
    ControlModGone(m);
    InteractModGone(m);
    HudModGone(m);
    StateModGone(m);
    AnimsModGone(m);
    HitsModGone(m);
    FrameLifeModGone(m);
    DrawWorldModGone(m);
    ScaleModGone(m);
    CapturesRemove(m);
    SidecarChannelsForget(m);
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
    // its files out of the game, but in a reload: the load that follows looks at them again (a name whose file did
    // not change is not told as changed)
    if (strcmp(why, "reload") != 0) FilesModOff(e->id);
    if (!e->dll) {   // a mod of files alone
        e->lastUnload = json{{"generation", e->generation}, {"why", why}};
        e->current = nullptr;
        Log(MML_LOG_INFO, "host", "mod %s unloaded (%s)", e->id.c_str(), why);
        EventsRecord("MOD_UNLOADED", json({{"name", e->id}, {"why", why}}).dump(-1, ' ', false, json::error_handler_t::replace));
        return true;
    }
    // A thread still in the load's code (one the mod started and did not stop, or a library's worker): freeing the
    // DLL would crash the game at that thread's next instruction. A little time for threads that are ending.
    std::vector<BusyThread> busy = ThreadsInDir(e->liveDir);
    for (ULONGLONG until = GetTickCount64() + 1000; !busy.empty() && GetTickCount64() < until;) {
        Sleep(25);
        busy = ThreadsInDir(e->liveDir);
    }
    json report = {{"generation", e->generation}, {"why", why}};
    if (!busy.empty()) {
        json threads = json::array();
        for (const BusyThread &t : busy) {
            Log(MML_LOG_ERROR, "host", "mod %s: thread %lu is still in its code (%s %s) after its unload: its DLL is kept "
                "loaded, not freed, until the thread leaves it (a mod stops its threads in mml_mod_unload)", e->id.c_str(),
                t.tid, t.how, t.where.c_str());
            threads.push_back({{"tid", t.tid}, {"how", t.how}, {"where", t.where}});
        }
        report["kept"] = true;
        report["threads"] = threads;
        g_kept.push_back(Kept{e->id, e->generation, e->dll, e->liveDir});
    } else {
        FreeLibrary(e->dll);
        std::vector<std::string> stays = ModulesInDir(e->liveDir);   // loaded by the mod and not freed, or held
        if (!stays.empty()) {
            std::string list;
            for (const std::string &s : stays) list += (list.empty() ? "" : ", ") + s;
            Log(MML_LOG_WARN, "host", "mod %s: still loaded after its unload: %s (loaded by the mod and never freed, or "
                "held by something it made)", e->id.c_str(), list.c_str());
            report["stays_loaded"] = stays;
        }
        LiveRemove(e->liveDir);
    }
    ProcessMeasure now = MeasureProcess();
    double freeMb = now.freeMb - e->atLoad.freeMb, largestMb = now.largestFreeMb - e->atLoad.largestFreeMb;
    int handles = now.handles - e->atLoad.handles, threads = now.threads - e->atLoad.threads;
    report["since_load"] = {{"free_mb", std::round(freeMb * 10) / 10}, {"largest_free_mb", std::round(largestMb * 10) / 10},
                            {"handles", handles}, {"threads", threads}};
    e->lastUnload = report;
    e->dll = nullptr;
    e->unload = nullptr;
    e->current = nullptr;
    // (the whole process's numbers over the load's life: the game's own changes are in them too)
    Log(MML_LOG_INFO, "host", "mod %s unloaded (%s); since its load: address space free %+.1f MB (largest block %+.1f MB), "
        "handles %+d, threads %+d", e->id.c_str(), why, freeMb, largestMb, handles, threads);
    EventsRecord("MOD_UNLOADED", json({{"name", e->id}, {"why", why}}).dump(-1, ' ', false, json::error_handler_t::replace));
    for (const std::string &s : gone) ServicesChanged(s);
    return true;
}

// The mods folder read again: new mods added, manifests of unloaded ones refreshed. Loaded mods keep their manifest
// until they are reloaded.
void Scan() {
    std::string root = MmlDir() + "mods\\";
    MakeDir(root);
    for (const FoundFile &fd : ListDir(root + "*")) {
        if (!fd.dir() || fd.name[0] == '.') continue;
        std::string dir = root + fd.name + "\\";
        ModEntry tmp;
        std::string err;
        if (!ReadManifest(dir, fd.name, &tmp, &err)) {
            HWARN("mods\\%s: %s", fd.name.c_str(), err.c_str());
            continue;
        }
        ModEntry *e = ById(tmp.id);
        if (!e) {
            g_mods.push_back(std::make_unique<ModEntry>());
            e = g_mods.back().get();
            e->id = tmp.id;
        }
        e->settings = tmp.settings;   // (metadata only: a loaded mod's page follows its manifest too)
        if (e->current) continue;
        e->name = tmp.name; e->version = tmp.version; e->type = tmp.type; e->entry = tmp.entry; e->entry64 = tmp.entry64; e->enabled = tmp.enabled;
        e->order = tmp.order; e->depends = tmp.depends; e->builds = tmp.builds; e->gfx = tmp.gfx; e->dir = tmp.dir;
    }
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
            if (LoadMod(e.get(), &err, true)) { e->error.clear(); any = true; }
            else e->error = err;
        }
        if (!any) break;
    }
    for (auto &e : g_mods)
        if (e->enabled && !e->current) {
            Log(MML_LOG_WARN, "host", "mod %s not loaded: %s", e->id.c_str(), e->error.c_str());
            FilesModOff(e->id);
        }
}

// Loads kept after their unload: freed once no thread is in their code any more (checked now and then).
void FreeKept() {
    for (auto it = g_kept.begin(); it != g_kept.end();) {
        if (!ThreadsInDir(it->liveDir).empty()) { ++it; continue; }
        FreeLibrary(it->dll);
        LiveRemove(it->liveDir);
        Log(MML_LOG_INFO, "host", "mod %s generation %u: no thread in its code any more, its DLL freed now", it->id.c_str(), it->generation);
        it = g_kept.erase(it);
    }
}

json ModValue(const ModEntry &e) {
    json j = {{"id", e.id}, {"name", e.name}, {"version", e.version}, {"type", e.type}, {"enabled", e.enabled},
              {"loaded", e.current != nullptr}, {"generation", e.generation}, {"load_order", e.order}, {"dir", e.dir},
              {"depends", e.depends}};
    if (!e.builds.empty()) j["builds"] = e.builds;
    if (e.gfx) j["gfx"] = true;
    if (!e.entry64.empty()) {
        std::string half = SidecarHalf(e.id);
        j["x64"] = half.empty() ? json({{"entry", e.entry64}, {"state", "not loaded"}}) : json::parse(half, nullptr, false);
    }
    if (e.current && !e.liveDir.empty()) j["live_dir"] = e.liveDir;
    if (size_t n = FilesCount(e.id)) j["files"] = n;
    if (!e.error.empty()) j["error"] = e.error;
    if (!e.lastUnload.is_null()) j["last_unload"] = e.lastUnload;
    json kept = json::array();
    for (const Kept &k : g_kept) if (k.id == e.id) kept.push_back(k.generation);
    if (!kept.empty()) j["kept_generations"] = kept;
    return j;
}
std::string ModJson(const ModEntry &e) { return ModValue(e).dump(-1, ' ', false, json::error_handler_t::replace); }
} // namespace

const char *ModIdOf(const MmlMod *mod) { return LoaderOwned(mod) ? "mml" : mod ? mod->entry->id.c_str() : "host"; }
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
std::string ModSettingsJson(const MmlMod *mod) {
    if (!Alive(mod) || mod->entry->settings.is_null()) return std::string();
    return mod->entry->settings.dump(-1, ' ', false, json::error_handler_t::replace);
}
std::string ModNameOf(const MmlMod *mod) { return Alive(mod) ? mod->entry->name : std::string(); }
std::vector<MmlMod *> ModsLoaded() {
    std::vector<MmlMod *> out;
    for (auto &e : g_mods) if (e->current && !e->faulted) out.push_back(e->current);
    return out;
}
// The same from any thread (a hook in the middle of a function may run on the game's sound or loading thread): the
// fault guard and the x87's precision, the profiler's times only on the game's thread.
bool ModCallAnyThread(MmlMod *mod, void (*fn)(void *), void *user, const char *where) {
    if (GetCurrentThreadId() == MainThread()) return ModCall(mod, fn, user, where);
    if (!Alive(mod) || mod->entry->faulted) return false;
    DWORD ex = SafePosted(fn, user);
    if (ex) { Faulted(mod, where, ex); return false; }
    return true;
}

bool ModCall(MmlMod *mod, void (*fn)(void *), void *user, const char *where) {
    if (!Alive(mod) || mod->entry->faulted) return false;
    DWORD ex;
    {
        ProfScope ps(mod, where);
        ex = SafePosted(fn, user);
    }
    if (ex) { Faulted(mod, where, ex); return false; }
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
    g_host.hook_mid = H_HookMid;
    g_host.code_patch = H_CodePatch;
    g_host.gfx_show_stats = H_GfxShowStats;
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
    g_host.script_run = H_ScriptRun;
    g_host.script_stop = H_ScriptStop;
    g_host.script_state = H_ScriptState;
    g_host.actor_spawn = H_ActorSpawn;
    g_host.actor_remove = H_ActorRemove;
    g_host.actor_teleport = H_ActorTeleport;
    g_host.item_id = H_ItemId;
    g_host.give_item = H_GiveItem;
    g_host.actor_seat = H_ActorSeat;
    g_host.file_add = H_FileAdd;
    g_host.file_source = H_FileSource;
    g_host.file_read = H_FileRead;
    g_host.render_device = RenderDevice;
    g_host.render_camera = RenderCamera;
    g_host.gfx_pass = H_GfxPass;
    g_host.gfx_pass_remove = H_GfxPassRemove;
    g_host.gfx_ui = H_GfxUi;
    g_host.scene_view = H_SceneView;
    g_host.scene_view_remove = H_SceneViewRemove;
    g_host.geom_hide = H_GeomHide;
    g_host.geom_show = H_GeomShow;
    g_host.geom_capture = H_GeomCapture;
    g_host.frame_stats = H_FrameStats;
    g_host.humans_list = H_HumansList;
    g_host.human_skeleton = H_HumanSkeleton;
    g_host.bone_set = H_BoneSet;
    g_host.human_player = H_HumanPlayer;
    g_host.camera_override = H_CameraOverride;
    g_host.camera_project = H_CameraProject;
    g_host.camera_ray = H_CameraRay;
    g_host.world_ray = H_WorldRay;
    g_host.world_sweep = H_WorldSweep;
    g_host.world_near = H_WorldNear;
    g_host.world_shapes = H_WorldShapes;
    g_host.money = H_Money;
    g_host.money_set = H_MoneySet;
    g_host.human_health = H_HumanHealth;
    g_host.human_health_set = H_HumanHealthSet;
    g_host.human_items = H_HumanItems;
    g_host.item_ammo_set = H_ItemAmmoSet;
    g_host.human_take_item = H_HumanTakeItem;
    g_host.scene_lights = H_SceneLights;
    g_host.light_set = H_LightSet;
    g_host.scene_sun = H_SceneSun;
    g_host.car_exhaust = H_CarExhaust;
    g_host.exhaust_hook = H_ExhaustHook;
    g_host.particle_def = H_ParticleDef;
    g_host.particle_def_set = H_ParticleDefSet;
    g_host.particle_hook = H_ParticleHook;
    g_host.weather_get = H_WeatherGet;
    g_host.weather_set = H_WeatherSet;
    g_host.weather_reset = H_WeatherReset;
    g_host.prop_create = H_PropCreate;
    g_host.prop_place = H_PropPlace;
    g_host.prop_attach = H_PropAttach;
    g_host.prop_show = H_PropShow;
    g_host.prop_remove = H_PropRemove;
    g_host.actor_frame = H_ActorFrame;
    g_host.frame_world = H_FrameWorld;
    g_host.sound_play = H_SoundPlay;
    g_host.sound_set = H_SoundSet;
    g_host.sound_move = H_SoundMove;
    g_host.sound_stop = H_SoundStop;
    g_host.sound_state = H_SoundState;
    g_host.music_play = H_MusicPlay;
    g_host.music_set = H_MusicSet;
    g_host.music_stop = H_MusicStop;
    g_host.music_game = H_MusicGame;
    g_host.key_capture = H_KeyCapture;
    g_host.action_capture = H_ActionCapture;
    g_host.player_lock = H_PlayerLock;
    g_host.vehicle_hook = H_VehicleHook;
    g_host.vehicle_body = H_VehicleBody;
    g_host.zone_add = H_ZoneAdd;
    g_host.use_point_add = H_UseAdd;
    g_host.timer_add = H_TimerAdd;
    g_host.interact_remove = H_InteractRemove;
    g_host.picture_load = H_PictureLoad;
    g_host.picture_size = H_PictureSize;
    g_host.picture_free = H_PictureFree;
    g_host.draw_picture = H_DrawPicture;
    g_host.hud_console = H_HudConsole;
    g_host.hud_help = H_HudHelp;
    g_host.hud_marker_add = H_HudMarkerAdd;
    g_host.hud_marker_set = H_HudMarkerSet;
    g_host.hud_marker_remove = H_HudMarkerRemove;
    g_host.save_put = H_SavePut;
    g_host.save_get = H_SaveGet;
    g_host.human_anim_play = H_HumanAnimPlay;
    g_host.human_anim_stop = H_HumanAnimStop;
    g_host.human_anim_replace = H_HumanAnimReplace;
    g_host.human_anim_state = H_HumanAnimState;
    g_host.human_get_up = H_HumanGetUp;
    g_host.item_define = H_ItemDefine;
    g_host.item_find = H_ItemFind;
    g_host.stats_get = H_StatsGet;
    g_host.profile_progress = H_ProfileProgress;
    g_host.profile_saves = H_ProfileSaves;
    g_host.world_water = H_WorldWater;
    g_host.zone_watch = H_ZoneWatch;
    g_host.prop_anim_play = H_PropAnimPlay;
    g_host.prop_anim_stop = H_PropAnimStop;
    g_host.prop_anim_state = H_PropAnimState;
    g_host.prop_physics = H_PropPhysics;
    g_host.prop_push = H_PropPush;
    g_host.prop_body = H_PropBody;
    g_host.game_files = H_GameFiles;
    g_host.model_file = H_ModelFile;
    g_host.model_open = H_ModelOpen;
    g_host.model_nodes = H_ModelNodes;
    g_host.model_mesh = H_ModelMesh;
    g_host.model_materials = H_ModelMaterials;
    g_host.model_close = H_ModelClose;
    g_host.scene_focus = H_SceneFocus;
    g_host.scene_pieces = H_ScenePieces;
    g_host.actor_meshes = H_ActorMeshes;
    g_host.shader_add = H_ShaderAdd;
    g_host.shader_remove = H_ShaderRemove;
    g_host.shader_shadows = H_ShaderShadows;
    g_host.game_camera = H_GameCamera;
    g_host.game_camera_mode = H_GameCameraMode;
    g_host.game_camera_fov = H_GameCameraFov;
    g_host.vehicle_kinds = H_VehicleKinds;
    g_host.human_order = H_HumanOrder;
    g_host.human_order_state = H_HumanOrderState;
    g_host.road_near = H_RoadNear;
    g_host.channel_open = H_ChannelOpen;
    g_host.channel_write = H_ChannelWrite;
    g_host.channel_read = H_ChannelRead;
    g_host.gfx_shared_texture = H_GfxSharedTexture;
    g_host.channel_wait = H_ChannelWait;
    g_host.human_invulnerable = H_HumanInvulnerable;
    g_host.hud_game = H_HudGame;
    g_host.web_nodes = H_WebNodes;
    g_host.material_name = H_MaterialName;
    g_host.gfx_state = H_GfxState;
    g_host.gfx_motion = H_GfxMotion;
    g_host.skin_set = H_SkinSet;
    g_host.skin_get = H_SkinGet;
    g_host.skin_gpu = H_SkinGpu;
    g_host.shader_constants = H_ShaderConstants;
    g_host.shader_vertex_data = H_ShaderVertexData;
    g_host.gfx_pass_views = H_GfxPassViews;
    g_host.hit_filter = H_HitFilter;
    g_host.human_hit = H_HumanHit;
    g_host.pedestrians_list = H_PedestriansList;
    g_host.bullet_shape = H_BulletShape;
    g_host.frame_watch = H_FrameWatch;
    g_host.draw_world = H_DrawWorld;
    g_host.scene_ready = H_SceneReady;
    g_host.actor_scale = H_ActorScale;
    g_host.scale_beat = H_ScaleBeat;
    g_host.gfx_shared_fence = H_GfxSharedFence;
    g_host.gfx_show = H_GfxShow;
    g_host.gfx_jitter = H_GfxJitter;
    g_host.gfx_upscale = H_GfxUpscale;
    LiveCleanAll();
    Scan();
    LoadEnabled();
}

void ModsEarlyFiles() {
    Scan();
    for (auto &e : g_mods)
        if (e->enabled && (e->type == "native" || e->type == "files") && ForThisBuild(*e)) FilesModOn(e->id, e->dir, e->order, true);
}

std::vector<std::string> ModsWantingGfx() {
    std::vector<std::string> ids;
    for (auto &e : g_mods)
        if (e->enabled && e->gfx && ForThisBuild(*e)) ids.push_back(e->id);
    return ids;
}

void ModsShutdown() {
    for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it) UnloadMod(it->get(), "the game is closing");
    SidecarShutdown();
}

bool Subscribed(MmlEventType type) {
    for (const Sub &s : g_subs)
        if (s.type == type && Alive(s.mod) && !s.mod->entry->faulted) return true;
    return false;
}

void Dispatch(const MmlEvent &ev) {
    EventsFromDispatch(ev);   // (events.cpp: kept for agents)
    StatsEvent(ev);           // (stats.cpp: what the player did)
    if (g_subs.empty()) return;
    std::vector<Sub> subs = g_subs;   // a handler may subscribe; a faulted mod is unloaded only at the next frame
    for (const Sub &s : subs) {
        if (s.type != ev.type || !Alive(s.mod) || s.mod->entry->faulted) continue;
        DWORD ex;
        {
            ProfScope ps(s.mod, ProfEventSite(ev.type));
            ex = SafeEvent(s.fn, &ev, s.user);
        }
        if (ex) Faulted(s.mod, ("its handler of event " + std::to_string(ev.type)).c_str(), ex);
    }
}

void DispatchTo(MmlMod *mod, const MmlEvent &ev) {
    std::vector<Sub> subs = g_subs;
    for (const Sub &s : subs) {
        if (s.mod != mod || s.type != ev.type || !Alive(s.mod) || s.mod->entry->faulted) continue;
        DWORD ex;
        {
            ProfScope ps(s.mod, ProfEventSite(ev.type));
            ex = SafeEvent(s.fn, &ev, s.user);
        }
        if (ex) Faulted(s.mod, ("its handler of event " + std::to_string(ev.type)).c_str(), ex);
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
        DWORD ex;
        {
            ProfScope ps(p.mod, "a posted call");
            ex = SafePosted(p.fn, p.user);
        }
        if (ex) Faulted(p.mod, "a posted call", ex);
    }
    for (auto &e : g_mods)
        if (e->current && e->faulted) UnloadMod(e.get(), "it faulted");
    static unsigned tick = 0;
    if (!g_kept.empty() && ++tick % 30 == 0) FreeKept();
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
    if (DrawWorldMethod(method, paramsJson, reply, &ok)) return ok;
    if (DrawMethod(method, paramsJson, reply, &ok)) return ok;
    if (ConfigMethod(method, paramsJson, reply, &ok)) return ok;
    if (MenusMethod(method, paramsJson, reply, &ok)) return ok;
    if (PanelMethod(method, paramsJson, reply, &ok)) return ok;
    if (FpuMethod(method, paramsJson, reply, &ok)) return ok;
    if (GameCameraMethod(method, paramsJson, reply, &ok)) return ok;
    if (ScriptMethod(method, paramsJson, reply, &ok)) return ok;
    if (CollideMethod(method, paramsJson, reply, &ok)) return ok;
    if (StateMethod(method, paramsJson, reply, &ok)) return ok;
    if (LightsMethod(method, paramsJson, reply, &ok)) return ok;
    if (EffectsMethod(method, paramsJson, reply, &ok)) return ok;
    if (PropsMethod(method, paramsJson, reply, &ok)) return ok;
    if (ModelsMethod(method, paramsJson, reply, &ok)) return ok;
    if (MeshesMethod(method, paramsJson, reply, &ok)) return ok;
    if (ShadersMethod(method, paramsJson, reply, &ok)) return ok;
    if (SkinMethod(method, paramsJson, reply, &ok)) return ok;
    if (SoundMethod(method, paramsJson, reply, &ok)) return ok;
    if (VideoMethod(method, paramsJson, reply, &ok)) return ok;
    if (ControlMethod(method, paramsJson, reply, &ok)) return ok;
    if (InteractMethod(method, paramsJson, reply, &ok)) return ok;
    if (HudMethod(method, paramsJson, reply, &ok)) return ok;
    if (SavesMethod(method, paramsJson, reply, &ok)) return ok;
    if (AnimsMethod(method, paramsJson, reply, &ok)) return ok;
    if (HitsMethod(method, paramsJson, reply, &ok)) return ok;
    if (PedestriansMethod(method, paramsJson, reply, &ok)) return ok;
    if (FrameLifeMethod(method, paramsJson, reply, &ok)) return ok;
    if (ScaleMethod(method, paramsJson, reply, &ok)) return ok;
    if (ItemsMethod(method, paramsJson, reply, &ok)) return ok;
    if (StatsMethod(method, paramsJson, reply, &ok)) return ok;
    if (WorldMethod(method, paramsJson, reply, &ok)) return ok;
    if (FilesMethod(method, paramsJson, reply, &ok)) return ok;
    if (RenderMethod(method, paramsJson, reply, &ok)) return ok;
    if (GfxMethod(method, paramsJson, reply, &ok)) return ok;
    if (ViewMethod(method, paramsJson, reply, &ok)) return ok;
    if (GeomMethod(method, paramsJson, reply, &ok)) return ok;
    if (ProfMethod(method, paramsJson, reply, &ok)) return ok;
    if (CrashMethod(method, paramsJson, reply, &ok)) return ok;
    if (NativeMethod(method, paramsJson, reply, &ok)) return ok;
    if (HumansMethod(method, paramsJson, reply, &ok)) return ok;
    if (OrdersMethod(method, paramsJson, reply, &ok)) return ok;
    if (WatchMethod(method, paramsJson, reply, &ok)) return ok;
    if (CameraMethod(method, paramsJson, reply, &ok)) return ok;
    if (SidecarMethod(method, paramsJson, reply, &ok)) return ok;
    if (method == "mods.list") { *reply = ModsList(); return true; }
    if (method == "commands.list") { *reply = CommandsList(); return true; }
    if (method == "hooks.list") { *reply = HookList(); return true; }
    if (method == "patches.list") { *reply = PatchList(); return true; }
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
        int times = params.is_object() && params.contains("times") && params["times"].is_number_integer() ? params["times"].get<int>() : 1;
        if (method == "mod.reload" && times != 1) {
            // reloaded again and again, the process measured after each load: what one reload does not give back
            if (times < 1 || times > 200) { *reply = "times: 1 to 200"; return false; }
            if (!e->current) { *reply = "mod " + id + " is not loaded"; return false; }
            auto r1 = [](double v) { return std::round(v * 10) / 10; };
            ProcessMeasure first = MeasureProcess(), last = first;
            json rounds = json::array();
            for (int i = 0; i < times; i++) {
                UnloadMod(e, "reload");
                Scan();
                std::string err;
                if (!LoadMod(e, &err)) {
                    e->error = err;
                    *reply = "mod " + id + ", reload " + std::to_string(i + 1) + " of " + std::to_string(times) + ": " + err;
                    return false;
                }
                last = MeasureProcess();
                rounds.push_back({{"generation", e->generation}, {"free_mb", r1(last.freeMb)}, {"largest_free_mb", r1(last.largestFreeMb)},
                                  {"handles", last.handles}, {"threads", last.threads}});
            }
            e->error.clear();
            json out = ModValue(*e);
            out["rounds"] = rounds;
            out["per_reload"] = {{"free_mb", std::round((last.freeMb - first.freeMb) / times * 100) / 100},
                                 {"largest_free_mb", std::round((last.largestFreeMb - first.largestFreeMb) / times * 100) / 100},
                                 {"handles", static_cast<double>(last.handles - first.handles) / times},
                                 {"threads", static_cast<double>(last.threads - first.threads) / times}};
            *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
            return true;
        }
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
    DWORD cex;
    {
        std::string site = "command " + method;
        ProfScope ps(c.mod, site.c_str());
        cex = SafeCommand(c.fn, args.c_str(), &r, c.user, &rc);
    }
    if (DWORD ex = cex) {
        Faulted(c.mod, method.c_str(), ex);
        *reply = "the command faulted (exception " + std::to_string(ex) + "); its mod is unloaded at the next frame";
        return false;
    }
    if (r.error || rc != 0) { *reply = r.text.empty() ? "command failed (" + std::to_string(rc) + ")" : r.text; return false; }
    *reply = r.text.empty() ? "null" : r.text;
    return true;
}

std::string ModAt(uintptr_t addr) {
    HMODULE m = nullptr;
    if (!addr || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<const char *>(addr), &m) || !m)
        return "";
    for (auto &e : g_mods) if (e->dll == m) return e->id;
    for (const Kept &k : g_kept) if (k.dll == m) return k.id + ", an old load";
    return "";
}

std::string ModsList() {
    std::string out = "[";
    for (size_t i = 0; i < g_mods.size(); i++) out += (i ? "," : "") + ModJson(*g_mods[i]);
    return out + "]";
}

// The host's methods, each with its params, its result and the thread that answers it (host/methods.inc, made by
// tools/gen_methods.py from docs/PROTOCOL.md), then every mod's commands with their help.
struct MethodDoc { const char *name, *thread, *params, *result; };
const MethodDoc kMethods[] = {
#include "methods.inc"
};

std::string CommandsList() {
    json out = json::array();
    for (const MethodDoc &m : kMethods)
        out.push_back({{"name", m.name}, {"owner", "host"}, {"thread", m.thread}, {"params", m.params}, {"result", m.result}});
    for (auto &kv : g_commands) out.push_back({{"name", kv.first}, {"owner", Who(kv.second.mod)}, {"help", kv.second.help}});
    for (auto &kv : SidecarCommands())
        out.push_back({{"name", kv.first}, {"owner", kv.first.substr(0, kv.first.find('.')) + " (x64)"}, {"help", kv.second}, {"thread", "sidecar"}});
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace host
