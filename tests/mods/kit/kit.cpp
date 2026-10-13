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
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
