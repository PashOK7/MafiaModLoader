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
#include "mml/path.h"

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

// The game's state with no game: no money, no person, nothing set.
int State(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, human_take_item)) { H->reply_error(r, "the loader has no game state"); return 1; }
    MmlItem items[4];
    float max = 0;
    char out[160];
    snprintf(out, sizeof out, "{\"money\":%d,\"money_set\":%d,\"health\":%.0f,\"health_set\":%d,\"items\":%d,\"ammo\":%d,\"take\":%d}",
             H->money(M) == INT32_MIN ? -1 : 0, H->money_set(M, 5), H->human_health(M, nullptr, &max), H->human_health_set(M, nullptr, 50),
             H->human_items(M, nullptr, items, 4), H->item_ammo_set(M, nullptr, 0, 1, 1), H->human_take_item(M, nullptr, 10));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The scene's lights with no game: none, nothing set, no sun.
int Lights(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, scene_sun)) { H->reply_error(r, "the loader has no scene lights"); return 1; }
    MmlLight l[2];
    float s[3], c[3], a[3];
    const float color[3] = {1, 0, 0};
    char out[96];
    snprintf(out, sizeof out, "{\"lights\":%d,\"set\":%d,\"sun\":%d}", H->scene_lights(M, l, 2), H->light_set(M, l, color, 1, NAN, NAN, -1),
             H->scene_sun(M, s, c, a));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The game's effects with no game: no exhaust, the hook refused (no build known), no particle records.
int NoExhaust(const MmlExhaust *, void *) { return 0; }
int NoParticle(MmlParticle *, void *) { return 0; }
int Effects(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, particle_def_set)) { H->reply_error(r, "the loader has no game effects"); return 1; }
    MmlExhaust e;
    MmlParticleDef d;
    char out[128];
    snprintf(out, sizeof out, "{\"exhaust\":%d,\"hook\":%d,\"defs\":%d,\"def\":%d,\"set\":%d,\"particles\":%d}", H->car_exhaust(M, nullptr, &e),
             H->exhaust_hook(M, NoExhaust, nullptr), H->particle_def(M, -1, nullptr), H->particle_def(M, 12, &d), H->particle_def_set(M, 12, &d),
             H->particle_hook(M, NoParticle, nullptr));
    H->exhaust_hook(M, nullptr, nullptr);
    H->particle_hook(M, nullptr, nullptr);
    double w = 0;
    if (H->weather_get(M, MML_WEATHER_ON, &w) || H->weather_set(M, MML_WEATHER_ON, 1) || H->weather_reset(M) ||
        H->weather_get(M, MML_WEATHER_PARAMS, &w)) {
        H->reply_error(r, "the weather answered with no game");
        return 1;
    }
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Props with no game: none made, nothing placed, no frame.
int Props(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, frame_world)) { H->reply_error(r, "the loader has no props"); return 1; }
    const float pos[3] = {0, 0, 0};
    float w[16];
    char out[160];
    void *p = H->prop_create(M, "2tommy.i3d", nullptr, pos, nullptr, 1);
    snprintf(out, sizeof out, "{\"create\":%d,\"place\":%d,\"attach\":%d,\"show\":%d,\"remove\":%d,\"actor_frame\":%d,\"frame_world\":%d}", p ? 1 : 0,
             H->prop_place(M, &w, pos, nullptr, 0), H->prop_attach(M, &w, nullptr, 1), H->prop_show(M, &w, 0), H->prop_remove(M, &w),
             H->actor_frame(M, nullptr) ? 1 : 0, H->frame_world(M, nullptr, w));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Sound with no game: nothing played, nothing known, the city's music not there.
int Sound(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, music_game)) { H->reply_error(r, "the loader has no sound"); return 1; }
    const float pos[3] = {0, 0, 0};
    MmlSoundState st{};
    st.size = sizeof st;
    char out[200];
    snprintf(out, sizeof out, "{\"play2d\":%d,\"play3d\":%d,\"set\":%d,\"move\":%d,\"stop\":%d,\"state\":%d,\"music\":%d,\"music_set\":%d,\"music_stop\":%d,\"music_game\":%d}",
             H->sound_play(M, "menuopen.wav", nullptr, nullptr, 1, 1, 0, 0, 0), H->sound_play(M, "menuopen.wav", nullptr, pos, 1, 1, 0, 0, MML_SOUND_LOOP),
             H->sound_set(M, 1, 0.5f, 1), H->sound_move(M, 1, nullptr, pos), H->sound_stop(M, 1), H->sound_state(M, 1, &st),
             H->music_play(M, "x.ogg", 1, 0), H->music_set(M, 1, 1), H->music_stop(M, 1), H->music_game(M, 0));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Control with no game: no key or slot kept, no lock, no vehicle hook, no body.
int ControlStep(MmlVehicleStep *, void *) { return 0; }
int Control(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, vehicle_body)) { H->reply_error(r, "the loader has no control"); return 1; }
    MmlVehicleBody b{};
    char out[200];
    snprintf(out, sizeof out, "{\"key\":%d,\"action\":%d,\"lock\":%d,\"hook\":%d,\"unhook\":%d,\"body\":%d,\"kinds\":%d}", H->key_capture(M, 17, 1),
             H->action_capture(M, 0, 1), H->player_lock(M, 1), H->vehicle_hook(M, ControlStep, nullptr), H->vehicle_hook(M, nullptr, nullptr),
             H->vehicle_body(M, nullptr, &b, nullptr), MML_HAS(H, vehicle_kinds) ? H->vehicle_kinds(M, MML_VEHICLES_CARS | MML_VEHICLES_AIRPLANES) : -1);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Interaction with no game: zones and use-points refused, a timer kept (the mission's clock: it never runs) and
// removed.
void KitZone(int, int, void *, void *) {}
void KitUse(int, void *) {}
void KitTimer(int, void *) {}
int Interact(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, interact_remove)) { H->reply_error(r, "the loader has no interaction"); return 1; }
    const float pos[3] = {0, 0, 0};
    int t = H->timer_add(M, 1000, 0, KitTimer, nullptr);
    int zone = H->zone_add(M, pos, 3, nullptr, KitZone, nullptr), use = H->use_point_add(M, pos, 2, nullptr, "Fish here", KitUse, nullptr);
    int removed = H->interact_remove(M, t), again = H->interact_remove(M, t);   // (in this order: not as arguments)
    char out[160];
    snprintf(out, sizeof out, "{\"zone\":%d,\"use\":%d,\"timer\":%s,\"removed\":%d,\"again\":%d}", zone, use, t > 0 ? "true" : "false", removed, again);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The HUD with no game: no picture, no line, no marker; drawing a picture that is not there is nothing.
int Hud(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, hud_marker_remove)) { H->reply_error(r, "the loader has no HUD functions"); return 1; }
    int pic = H->picture_load(M, "2sight.bmp", "2sight+.bmp");
    int w = -1, h = -1;
    int size = H->picture_size(pic, &w, &h);
    H->draw_picture(M, 7, 10, 10, 32, 32, 45, 0xFFFFFFFFu, nullptr);
    H->picture_free(M, 7);
    MmlHudMarker m{};
    m.size = sizeof m;
    m.flags = MML_MARKER_SCREEN | MML_MARKER_COMPASS;
    m.label = "Here";
    int marker = H->hud_marker_add(M, &m);
    int set = H->hud_marker_set(M, 1, &m), removed = H->hud_marker_remove(M, 1);
    int console = H->hud_console(M, "hello", 0xFFFFFFFFu), help = H->hud_help(M, "hello");
    char out[200];
    snprintf(out, sizeof out, "{\"picture\":%d,\"size\":%d,\"marker\":%d,\"set\":%d,\"removed\":%d,\"console\":%d,\"help\":%d}", pic, size, marker,
             set, removed, console, help);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The saves with no game: the slot's state kept (written with the game's next save), read back, removed; the
// profile's refused (no profile is selected).
int Saves(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, save_get)) { H->reply_error(r, "the loader has no save_put"); return 1; }
    char buf[16] = {0};
    int put = H->save_put(M, MML_SAVE_SLOT, "fish", "trout", 5);
    size_t got = H->save_get(M, MML_SAVE_SLOT, "fish", buf, sizeof buf);
    int profile = H->save_put(M, MML_SAVE_PROFILE, "caught", "1", 1);
    int removed = H->save_put(M, MML_SAVE_SLOT, "fish", nullptr, 0);
    size_t after = H->save_get(M, MML_SAVE_SLOT, "fish", nullptr, 0);
    int bad = H->save_put(M, 7, "x", "y", 1);
    char out[200];
    snprintf(out, sizeof out, "{\"put\":%d,\"got\":%u,\"value\":\"%s\",\"profile\":%d,\"removed\":%d,\"after\":%u,\"bad_scope\":%d}", put,
             static_cast<unsigned>(got), buf, profile, removed, static_cast<unsigned>(after), bad);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Animations with no game: every call refused (no person is known), the state not filled.
int Anims(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, human_get_up)) { H->reply_error(r, "the loader has no human_get_up"); return 1; }
    void *nobody = reinterpret_cast<void *>(static_cast<uintptr_t>(0x1000));
    MmlHumanAnim s{};
    s.size = sizeof s;
    s.playing = 7;
    int play = H->human_anim_play(M, nobody, "walk", 0), stop = H->human_anim_stop(M, nobody);
    int replace = H->human_anim_replace(M, nobody, "walk", "run"), state = H->human_anim_state(M, nobody, &s);
    int up = H->human_get_up(M, nobody, 500);
    char out[200];
    snprintf(out, sizeof out, "{\"play\":%d,\"stop\":%d,\"replace\":%d,\"state\":%d,\"untouched\":%d,\"get_up\":%d}", play, stop, replace, state,
             s.playing == 7 ? 1 : 0, up);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Hits before the game with no game: the filter set (taken away with {"off": true}), the game's Hit refused, no
// pedestrians.
int KitHitFilter(void *, int, MmlHumanHit *, void *) { return MML_HIT_GAME; }
void KitGone(void *, void *) {}
int Hits(const char *args, MmlReply *r, void *) {
    if (!MML_HAS(H, pedestrians_list)) { H->reply_error(r, "the loader has no hit_filter"); return 1; }
    bool off = args && strstr(args, "off");
    int filter = H->hit_filter(M, off ? nullptr : KitHitFilter, nullptr);
    void *nobody = reinterpret_cast<void *>(static_cast<uintptr_t>(0x1000));
    MmlHumanHit h{};
    h.type = 8;
    MmlPedestrian peds[4];
    int hit = H->human_hit(M, nobody, &h), nullHit = H->human_hit(M, nobody, nullptr), pedestrians = H->pedestrians_list(M, peds, 4);
    MmlCapsule c = {{0, 0, 0}, {0, 1, 0}, 0.2f};
    int shape = H->bullet_shape(M, nobody, &c, 1);
    int watch = H->frame_watch(M, nobody, KitGone, nullptr), unwatch = H->frame_watch(M, nobody, nullptr, nullptr);
    MmlDrawShape ds = {sizeof ds};
    ds.kind = MML_DRAW_SPHERE;
    ds.radius = 0.5f;
    ds.ms = 60000;
    MmlDrawShape bad = ds;
    bad.radius = -1;
    int drawn = H->draw_world(M, &ds), badDrawn = H->draw_world(M, &bad);
    const float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
    int ready = H->scene_ready(M, lo, hi);
    MmlActorScale sc{};
    sc.size = sizeof sc;
    sc.scale[0] = sc.scale[1] = sc.scale[2] = 2;
    int scaled = H->actor_scale(M, nullptr, &sc) + 10 * H->scale_beat(M);   // (no actor: refused; the beat: 1)
    char out[300];
    snprintf(out, sizeof out, "{\"filter\":%d,\"hit\":%d,\"null_hit\":%d,\"pedestrians\":%d,\"shape\":%d,\"watch\":%d,\"unwatch\":%d,\"draw\":%d,\"bad_draw\":%d,\"ready\":%d,\"scaled\":%d}",
             filter, hit, nullHit, pedestrians, shape, watch, unwatch, drawn, badDrawn, ready, scaled);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// A mod's items with no game: refused (no table), none found.
int Items(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, item_find)) { H->reply_error(r, "the loader has no item_define"); return 1; }
    MmlItemDef d{sizeof d, "rod", "Fishing rod", "2rod.i3d", 17, -1, nullptr};
    MmlItemDef bad{sizeof bad, "a rod!", nullptr, nullptr, 0, -1, nullptr};
    int define = H->item_define(M, &d), badName = H->item_define(M, &bad), null = H->item_define(M, nullptr);
    int find = H->item_find(M, "rod"), findOther = H->item_find(M, "other/rod");
    char out[200];
    snprintf(out, sizeof out, "{\"define\":%d,\"bad_name\":%d,\"null\":%d,\"find\":%d,\"find_other\":%d}", define, badName, null, find, findOther);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// Statistics and the progress with no game: the game's counts at 0, no profile, no saves.
int Stats(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, profile_saves)) { H->reply_error(r, "the loader has no stats_get"); return 1; }
    MmlStats s{};
    s.size = sizeof s;
    s.kills = 7;
    int game = H->stats_get(M, MML_SAVE_SLOT, &s), profile = H->stats_get(M, MML_SAVE_PROFILE, &s), bad = H->stats_get(M, 9, &s);
    MmlProgress p{};
    p.size = sizeof p;
    int progress = H->profile_progress(M, &p), saves = H->profile_saves(M, nullptr, 0);
    char out[220];
    snprintf(out, sizeof out, "{\"game\":%d,\"kills\":%d,\"profile\":%d,\"bad_scope\":%d,\"progress\":%d,\"profile_number\":%d,\"saves\":%d}", game,
             s.kills, profile, bad, progress, p.profile, saves);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The water and zone_watch with no game: none, refused.
int Water(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, zone_watch)) { H->reply_error(r, "the loader has no world_water"); return 1; }
    const float at[3] = {0, 0, 0};
    MmlWater w{};
    w.size = sizeof w;
    w.height = 7;
    int water = H->world_water(M, at, &w), watch = H->zone_watch(M, 12345, MML_ZONE_PEOPLE, nullptr);
    char out[120];
    snprintf(out, sizeof out, "{\"water\":%d,\"untouched\":%d,\"watch\":%d}", water, w.height == 7 ? 1 : 0, watch);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// A prop's animation and physics with no game: not a prop, refused.
int PropMore(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, prop_body)) { H->reply_error(r, "the loader has no prop_body"); return 1; }
    void *nope = reinterpret_cast<void *>(static_cast<uintptr_t>(0x1000));
    MmlPropAnim a{};
    a.time_ms = 7;
    MmlPropPhysics p{};
    p.mass = 5;
    const float impulse[3] = {0, 1, 0};
    int play = H->prop_anim_play(M, nope, "walk1", MML_PROP_ANIM_LOOP, 1), stop = H->prop_anim_stop(M, nope),
        state = H->prop_anim_state(M, nope, &a), phys = H->prop_physics(M, nope, &p), push = H->prop_push(M, nope, impulse, nullptr);
    char out[160];
    snprintf(out, sizeof out, "{\"play\":%d,\"stop\":%d,\"state\":%d,\"untouched\":%d,\"physics\":%d,\"push\":%d,\"body\":%d}", play, stop, state,
             a.time_ms == 7 ? 1 : 0, phys, push, H->prop_body(M, nope) ? 1 : 0);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The x87's precision as a mod's callback runs: its control word and 1/3 divided on the x87 itself (at 24 bits it
// would be the float nearest a third).
int Fpu(const char *, MmlReply *r, void *) {
    unsigned short cw;
    double one = 1, three = 3, third;
    __asm {
        fnstcw cw
        fld one
        fdiv three
        fstp third
    }
    int pc = (cw >> 8) & 3;
    char out[96];
    snprintf(out, sizeof out, "{\"bits\":%d,\"third\":\"%.17g\"}", pc == 0 ? 24 : pc == 2 ? 53 : pc == 3 ? 64 : 0, third);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The game's camera with no game: no state; a mode and a field of view refused.
int GameCam(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, game_camera_fov)) { H->reply_error(r, "the loader has no game_camera_fov"); return 1; }
    MmlGameCamera c{};
    c.size = sizeof c;
    char out[96];
    snprintf(out, sizeof out, "{\"state\":%d,\"mode\":%d,\"fov\":%d}", H->game_camera(M, &c), H->game_camera_mode(M, MML_GAMECAM_EYES),
             H->game_camera_fov(M, 1.5f));
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// The game's files and models with no game: no archives (the mods' files only), no model.
int Models(const char *, MmlReply *r, void *) {
    if (!MML_HAS(H, model_close)) { H->reply_error(r, "the loader has no model_close"); return 1; }
    int seen = 0;
    int all = H->game_files(M, "*", [](const char *, const char *, void *u) { ++*static_cast<int *>(u); return 0; }, &seen);
    char file[32] = "untouched";
    MmlModelNode n{};
    MmlModelMesh mesh{};
    int open = H->model_open(M, "nothing_here"), nodes = H->model_nodes(M, 12345, &n, 1), got = H->model_mesh(M, 12345, 0, 0, &mesh),
        mats = H->model_materials(M, 12345, nullptr, 0), mf = H->model_file(M, nullptr, file, sizeof file);
    H->model_close(M, 12345);
    const float lo[3] = {-10, -10, -10}, hi[3] = {10, 10, 10};
    MmlMeshPiece piece{};
    int focus = H->scene_focus(M, lo, hi), pieces = H->scene_pieces(M, &piece, 1),
        actor = H->actor_meshes(M, reinterpret_cast<void *>(static_cast<uintptr_t>(0x1000)), &piece, 1), off = H->scene_focus(M, nullptr, nullptr);
    MmlShadowSettings ss{1024, 40.0f, 0.003f};
    int shadows = MML_HAS(H, shader_shadows) ? H->shader_shadows(M, &ss) + H->shader_shadows(M, nullptr) : -1;
    char out[300];
    snprintf(out, sizeof out, "{\"files\":%d,\"told\":%d,\"open\":%d,\"nodes\":%d,\"mesh\":%d,\"materials\":%d,\"file\":%d,\"untouched\":%d,"
             "\"focus\":%d,\"pieces\":%d,\"actor\":%d,\"off\":%d,\"shadows\":%d}", all, seen, open, nodes, got, mats, mf,
             strcmp(file, "untouched") == 0 ? 1 : 0, focus, pieces, actor, off, shadows);
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
    FILE *f = mml_fopen(path.c_str(), "w");
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
    host->command(mod, "props", "prop_create and the rest with no game", Props, nullptr);
    host->command(mod, "interact", "zone_add, use_point_add, timer_add, interact_remove with no game", Interact, nullptr);
    host->command(mod, "hud", "picture_load and the rest, hud_console, hud_help, the markers with no game", Hud, nullptr);
    host->command(mod, "saves", "save_put, save_get with no game", Saves, nullptr);
    host->command(mod, "anims", "human_anim_play and the rest, human_get_up with no game", Anims, nullptr);
    host->command(mod, "items", "item_define, item_find with no game", Items, nullptr);
    host->command(mod, "stats", "stats_get, profile_progress, profile_saves with no game", Stats, nullptr);
    host->command(mod, "water", "world_water, zone_watch with no game", Water, nullptr);
    host->command(mod, "prop_more", "a prop's animation and physics with no game", PropMore, nullptr);
    host->command(mod, "models", "the game's files and models with no game", Models, nullptr);
    host->command(mod, "fpu", "the x87's precision in a mod's callback: 1/3 divided on the x87", Fpu, nullptr);
    host->command(mod, "gamecam", "the game's camera with no game: none, refused", GameCam, nullptr);
    host->command(mod, "hits", "hit_filter ({\"off\": true}: taken away), human_hit, pedestrians_list with no game", Hits, nullptr);
    host->command(mod, "control","key_capture, action_capture, player_lock, vehicle_hook, vehicle_body with no game", Control, nullptr);
    host->command(mod, "sound", "sound_play and the rest, music_play and the rest with no game", Sound, nullptr);
    host->command(mod, "effects","car_exhaust, exhaust_hook, particle_def(_set) with no game", Effects, nullptr);
    host->command(mod, "lights","scene_lights, light_set, scene_sun with no game", Lights, nullptr);
    host->command(mod, "state","money, money_set, human_health(_set), human_items, item_ammo_set, human_take_item with no game", State, nullptr);
    host->command(mod, "ask","world_ray, world_sweep, world_near, world_shapes with no game", WorldAsk, nullptr);
    host->command(mod, "camera","camera_override taken and given back, camera_project and camera_ray with no frame", Camera, nullptr);
    host->command(mod, "poke","{}: adds 7 to a field on the game thread; {\"addr\": 1}: the field's address", Poke, nullptr);
    host->command(mod, "natives","the addresses of native.call's test functions (each convention, floats, a buffer, a fault)", Natives, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
