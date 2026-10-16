// Test mod: the mod author's kit with no game (tests/run_tests.py): it queues drawing every frame, watches a key and
// two hotkeys (one from its settings, bound again when they change), reads its settings and writes into its data
// folder. `kit.status` tells what it saw; `kit.set_speed {"value": 30}` changes a setting through the host;
// `kit.write` writes a file into the data folder. It asks for an item in the pause menu at its load, and
// `kit.menu` tries to open a page of its own (with no game there are no menus: refused).
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
int g_keyF5;
unsigned g_frames, g_f5Down, g_f5Pressed, g_hotCtrlF6, g_hotSetting, g_configEvents, g_missionEvents;
std::string g_configEventName;
int g_menuItem;

void OnCtrlF6(void *) { g_hotCtrlF6++; }
void OnSetting(void *) { g_hotSetting++; }

void Bind() { H->hotkey(M, H->config_string(M, "hotkey", "F7"), OnSetting, nullptr); }

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) {
        g_configEvents++;
        g_configEventName = ev->name ? ev->name : "";
        Bind();   // the same function again: its combination is replaced
        return;
    }
    if (ev->type != MML_EVENT_FRAME) { g_missionEvents++; return; }
    g_frames++;
    if (H->key_down(g_keyF5)) g_f5Down++;
    if (H->key_pressed(g_keyF5)) g_f5Pressed++;
    H->draw_rect(M, 10, 10, 200, 40, 0x80000000u, 1);
    H->draw_rect(M, 10, 10, 200, 40, 0xFFFFFFFFu, 0);
    H->draw_line(M, 10, 50, 210, 50, 0xFFFF0000u);
    H->draw_text(M, 20, 15, 20, 0xFFFFFFFFu, MML_FONT_BIG, MML_DRAW_LEFT, "kit");
}

int Status(const char *, MmlReply *r, void *) {
    char out[1024];
    const char *overlay = H->config_json(M, "overlay");
    snprintf(out, sizeof out,
             "{\"frames\":%u,\"f5_down\":%u,\"f5_pressed\":%u,\"hot_ctrl_f6\":%u,\"hot_setting\":%u,\"config_events\":%u,"
             "\"config_event_name\":\"%s\",\"mission_events\":%u,\"speed\":%g,\"on\":%g,\"missing\":%g,\"name\":\"%s\","
             "\"hotkey\":\"%s\",\"overlay_x\":%g,\"overlay\":%s,\"mission\":\"%s\",\"mission_state\":%d,"
             "\"slot_accelerate\":%d,\"key_f5\":%d,\"key_bad\":%d,\"text_width\":%g,\"has_kit\":%d,\"menu_item\":%d}",
             g_frames, g_f5Down, g_f5Pressed, g_hotCtrlF6, g_hotSetting, g_configEvents, g_configEventName.c_str(), g_missionEvents,
             H->config_number(M, "speed", -1), H->config_number(M, "overlay.on", -1), H->config_number(M, "nothing.here", -7),
             H->config_string(M, "name", "?"), H->config_string(M, "hotkey", "?"), H->config_number(M, "overlay.x", -1),
             overlay ? overlay : "null", H->mission_name(), H->mission_state(), H->action_slot("Accelerate"), g_keyF5,
             H->key_code("NoSuchKey"), H->draw_text_width(MML_FONT_BIG, 20, "kit"), MML_HAS(H, config_set) ? 1 : 0, g_menuItem);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

int SetSpeed(const char *args, MmlReply *r, void *) {
    const char *v = strstr(args, "\"value\":");
    if (!v) { H->reply_error(r, "value: a number"); return 1; }
    char value[64];
    snprintf(value, sizeof value, "%g", strtod(v + 8, nullptr));
    H->reply_append(r, H->config_set(M, "speed", value) ? "true" : "false", static_cast<size_t>(-1));
    return 0;
}

void OnMenu(int, void *) {}

int Menu(const char *, MmlReply *r, void *) {
    const MmlMenuControl page[] = {{1, "wind", 40, 180, 300, 200, "Kit:", 0}, {10, "pbar", 20, 20, 100, 10, nullptr, 0},
                                   {400, "butt", 0, 120, 300, 20, "Back", 0}, {0, "end", 0, 0, 0, 0, nullptr, 0}};
    int open = H->menu_open(M, "page", page, 4, OnMenu, nullptr);
    float v = -1;
    char out[128];
    snprintf(out, sizeof out, "{\"open\":%d,\"set_value\":%d,\"value\":%d,\"set_text\":%d}", open, H->menu_set_value(M, 10, 0.5f),
             H->menu_value(M, 10, &v), H->menu_set_text(M, 400, "Back"));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

void OnScript(const MmlScriptResult *, void *) {}

// script_run with no game: refused (0); script_state of no such handle: -1.
int Script(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, script_state)) { H->reply_error(r, "the loader has no script machine"); return 1; }
    const float flts[1] = {1.5f};
    int h = H->script_run(M, "let flt[0] = 2\nend", nullptr, nullptr, 0, flts, 1, 1000, OnScript, nullptr);
    char out[96];
    snprintf(out, sizeof out, "{\"run\":%d,\"state\":%d}", h, H->script_state(M, 12345));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The world with no game: nothing made, moved, given or seated; no item known.
int World(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, actor_seat)) { H->reply_error(r, "the loader has no world functions"); return 1; }
    const MmlVec3 p{1, 2, 3};
    void *a = H->actor_spawn(M, MML_ACTOR_CAR, "fordttud00.i3d", &p, nullptr);
    char out[160];
    snprintf(out, sizeof out, "{\"spawn\":%d,\"remove\":%d,\"teleport\":%d,\"item\":%d,\"give\":%d,\"seat\":%d}", a ? 1 : 0,
             H->actor_remove(M, &out), H->actor_teleport(M, nullptr, &p, nullptr), H->item_id("Thompson"),
             H->give_item(M, nullptr, 10, -1, -1), H->actor_seat(M, nullptr, nullptr, 1));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Scene views: refusals (no function, a size out of range, one side 0), then as many as the host takes (eight), all
// taken back but the first, which stays for view.state.
int PlaceView(MmlViewCamera *, void *) { return 1; }
int Views(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, scene_view_remove)) { H->reply_error(r, "the loader has no scene views"); return 1; }
    int noFn = H->scene_view(M, 64, 32, nullptr, nullptr), tooBig = H->scene_view(M, 9000, 32, PlaceView, nullptr),
        half = H->scene_view(M, 64, 0, PlaceView, nullptr);
    int made[10] = {}, n = 0;
    for (int i = 0; i < 10; i++) if ((made[i] = H->scene_view(M, i ? 64 : 0, i ? 32 : 0, PlaceView, nullptr)) > 0) n++;
    for (int i = 1; i < 10; i++) if (made[i] > 0) H->scene_view_remove(M, made[i]);
    char out[160];
    snprintf(out, sizeof out, "{\"no_fn\":%d,\"too_big\":%d,\"half\":%d,\"made\":%d,\"first\":%d}", noFn, tooBig, half, n, made[0]);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// gfx_pass_views: on for a pass of the mod's (1), for no such pass (0); the pass kept for gfx.state, taken back by
// passviews_clear.
void NoPass(const MmlGfxFrame *, void *) {}
int g_viewPass;
int PassViews(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, gfx_pass_views)) { H->reply_error(r, "the loader has no gfx_pass_views"); return 1; }
    g_viewPass = H->gfx_pass(M, 500, NoPass, nullptr);
    char out[96];
    snprintf(out, sizeof out, "{\"pass\":%d,\"on\":%d,\"none\":%d}", g_viewPass, H->gfx_pass_views(M, g_viewPass, 1),
             H->gfx_pass_views(M, 99999, 1));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int PassViewsClear(const char *, MmlReply *r, void *) {
    if (g_viewPass) H->gfx_pass_remove(M, g_viewPass);
    g_viewPass = 0;
    H->reply_append(r, "{}", 2);
    return 0;
}

// The game's geometry with no game: geom_hide and geom_capture refused (0), geom_show of nothing harmless.
void OnGeom(const MmlGeomCapture *, void *) {}
int Geom(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, geom_capture)) { H->reply_error(r, "the loader has no geom_capture"); return 1; }
    char out[96];
    int hide = H->geom_hide(M, "00test*", 0), capture = H->geom_capture(M, "00test*", OnGeom, nullptr),
        empty = H->geom_hide(M, "", 0), nofn = H->geom_capture(M, "x", nullptr, nullptr);
    H->geom_show(M, 0);
    snprintf(out, sizeof out, "{\"hide\":%d,\"capture\":%d,\"empty\":%d,\"no_fn\":%d}", hide, capture, empty, nofn);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// native.call's subjects: one function a convention, floats, an out-buffer, a string, a fault.
int __cdecl NatAdd(int a, int b) { return a + b; }
int __stdcall NatMul(int a, int b) { return a * b; }
struct NatObj { int x; int __thiscall Plus(int a) { return x + a; } };
int __fastcall NatSum3(int a, int b, int c) { return a * 100 + b * 10 + c; }
float __stdcall NatHalf(float f) { return f * 0.5f; }
double __cdecl NatTwice(double d) { return d * 2; }
void __stdcall NatFill(int *out, int n) { for (int i = 0; i < n; i++) out[i] = i * 3; }
int __cdecl NatLen(const char *s) { return static_cast<int>(strlen(s)); }
int __stdcall NatFault(int *p) { return *p; }
NatObj g_natObj{40};
int Natives(const char *, MmlReply *r, void *) {
    int (__thiscall NatObj::*plus)(int) = &NatObj::Plus;
    void *plusAddr;
    memcpy(&plusAddr, &plus, sizeof plusAddr);
    char out[512];
    snprintf(out, sizeof out,
             "{\"add\":\"0x%p\",\"mul\":\"0x%p\",\"plus\":\"0x%p\",\"obj\":\"0x%p\",\"sum3\":\"0x%p\",\"half\":\"0x%p\",\"twice\":\"0x%p\","
             "\"fill\":\"0x%p\",\"len\":\"0x%p\",\"fault\":\"0x%p\"}",
             reinterpret_cast<void *>(&NatAdd), reinterpret_cast<void *>(&NatMul), plusAddr, static_cast<void *>(&g_natObj),
             reinterpret_cast<void *>(&NatSum3), reinterpret_cast<void *>(&NatHalf), reinterpret_cast<void *>(&NatTwice),
             reinterpret_cast<void *>(&NatFill), reinterpret_cast<void *>(&NatLen), reinterpret_cast<void *>(&NatFault));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The camera with no game: an override taken and given back; no frame to project on or cast a ray through.
int PlaceNothing(MmlViewCamera *, void *) { return 0; }
int Camera(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, camera_ray)) { H->reply_error(r, "the loader has no camera functions"); return 1; }
    const float w[3] = {1, 2, 3};
    float s[3], o[3], d[3];
    char out[128];
    int take = H->camera_override(M, PlaceNothing, nullptr), project = H->camera_project(M, w, s), ray = H->camera_ray(M, 400, 300, o, d),
        back = H->camera_override(M, nullptr, nullptr);
    snprintf(out, sizeof out, "{\"override\":%d,\"project\":%d,\"ray\":%d,\"back\":%d}", take, project, ray, back);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The world's questions with no game: no hit, no actor, no shape.
int WorldAsk(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, world_shapes)) { H->reply_error(r, "the loader has no world questions"); return 1; }
    const float a[3] = {0, 10, 0}, b[3] = {0, -10, 0};
    MmlHit hit;
    void *nearby[4];
    MmlShape shapes[4];
    char out[128];
    snprintf(out, sizeof out, "{\"ray\":%d,\"sweep\":%d,\"near\":%d,\"shapes\":%d}", H->world_ray(M, a, b, MML_WORLD_ALL, nullptr, &hit),
             H->world_sweep(M, a, b, 0.5f, MML_WORLD_ALL, nullptr, &hit), H->world_near(M, a, 100, 0, nearby, 4),
             H->world_shapes(M, b, a, MML_WORLD_ALL, shapes, 4));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// People with no game: none, no skeleton, bone_set refused, no player.
int Humans(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, human_player)) { H->reply_error(r, "the loader has no human_player"); return 1; }
    void *list[4];
    MmlBone b[MML_BONE_COUNT];
    char out[128];
    snprintf(out, sizeof out, "{\"count\":%d,\"skeleton\":%d,\"bone_set\":%d,\"player\":%d}", H->humans_list(M, list, 4),
             H->human_skeleton(M, nullptr, b, MML_BONE_COUNT), H->bone_set(M, nullptr, nullptr, nullptr), H->human_player(M) ? 1 : 0);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// A field written on the game thread (a command): what a hardware watchpoint finds.
volatile uint32_t g_watched;
int Poke(const char *args, MmlReply *r, void *) {
    char out[96];
    if (args && strstr(args, "\"addr\"")) {
        snprintf(out, sizeof out, "\"0x%08X\"", static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_watched)));
    } else {
        g_watched = g_watched + 7;
        snprintf(out, sizeof out, "%u", static_cast<unsigned>(g_watched));
    }
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

int Write(const char *, MmlReply *r, void *) {
    std::string path = std::string(H->mod_data_dir(M)) + "note.txt";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) { H->reply_error(r, "cannot write into the data folder"); return 1; }
    fputs("kit was here\n", f);
    fclose(f);
    std::string out = "\"";
    for (char c : path) { if (c == '\\') out += '\\'; out += c; }
    out += "\"";
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, config_set)) return 1;   // a loader older than the kit
    g_keyF5 = host->key_code("F5");
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_OPEN, MML_EVENT_MISSION_LOADED,
                           MML_EVENT_GAME_PAUSED, MML_EVENT_GAME_RESUMED})
        host->subscribe(mod, t, OnEvent, nullptr);
    if (!host->hotkey(mod, "Ctrl+F6", OnCtrlF6, nullptr)) return 2;
    if (host->hotkey(mod, "Ctrl+NoSuchKey", OnCtrlF6, reinterpret_cast<void *>(1))) return 3;   // must be refused
    Bind();
    host->command(mod, "status", "what the kit's parts gave this mod", Status, nullptr);
    host->command(mod, "set_speed", "config_set speed (params: value)", SetSpeed, nullptr);
    g_menuItem = MML_HAS(host, menu_value) ? host->menu_item(mod, "gamemenu", 108, "Kit", OnMenu, nullptr) : -1;
    host->command(mod, "menu", "tries to open a page of its own; what menu_open, menu_set_value, menu_value, menu_set_text gave", Menu, nullptr);
    host->command(mod, "write", "writes note.txt into the mod's data folder; its path", Write, nullptr);
    host->command(mod, "script", "tries script_run (no game: refused) and script_state", Script, nullptr);
    host->command(mod, "world", "tries actor_spawn, actor_remove, actor_teleport, item_id, give_item, actor_seat (no game: refused)", World, nullptr);
    host->command(mod, "views", "scene_view's refusals, its limit of eight, scene_view_remove (one view kept)", Views, nullptr);
    host->command(mod, "passviews", "a pass of its own, gfx_pass_views on it and on no such pass", PassViews, nullptr);
    host->command(mod, "passviews_clear", "takes that pass back", PassViewsClear, nullptr);
    host->command(mod, "geom", "geom_hide, geom_capture with no game (refused), geom_show", Geom, nullptr);
    host->command(mod, "humans", "humans_list, human_skeleton, bone_set, human_player with no game", Humans, nullptr);
    host->command(mod, "ask", "world_ray, world_sweep, world_near, world_shapes with no game", WorldAsk, nullptr);
    host->command(mod, "camera","camera_override taken and given back, camera_project and camera_ray with no frame", Camera, nullptr);
    host->command(mod, "poke","{}: adds 7 to a field on the game thread; {\"addr\": 1}: the field's address", Poke, nullptr);
    host->command(mod, "natives","the addresses of native.call's test functions (each convention, floats, a buffer, a fault)", Natives, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
