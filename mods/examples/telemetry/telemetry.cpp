// telemetry: a complete mod that uses every part of the mod author's kit once. Read it top to bottom, then copy it
// (tools/new_mod.py <id> makes the empty frame).
//
// What it does: an overlay with the car's speed, gear, engine speed, throttle and brake, and a lap timer between two
// checkpoints (a start and a finish, circles on the ground). The lap starts when the car leaves the start circle, or
// at once when the hotkey is pressed; it ends in the finish circle. The best lap of each mission is kept in the mod's
// data folder. Everything adjustable is a setting (config.json): agents change settings over MCP
// (`config.set {"mod": "telemetry", "key": "units", "value": "mph"}`), people edit the file; both take effect at once.
//
// The kit's parts, where each is used:
//   drawing            Draw()            text in the game's fonts, rectangles, lines; queued every frame
//   keys and actions   OnFrame, Bind()   a hotkey from the settings, a key held, the game's Accelerate / Brake actions
//   the car            OnTick, Draw      game::PlayerCar, game::Car (mml/game/game.h)
//   the mission        OnMission         opened, loaded, paused: the lap is dropped with the mission, the best read
//   files, settings    ReadSettings, LoadBest, SaveBest   config_*, mod_data_dir, MML_EVENT_CONFIG_CHANGED
//   the game's menus   OnMenuItem, OpenPage, OnPage   an item in the pause menu, a page of the mod's own (menu_*)
//   commands           telemetry.status, telemetry.lap, telemetry.checkpoint   (for agents, over MCP)
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace {
const MmlHost *H;
MmlMod *M;

// ---- settings (config.json: the defaults are next to the DLL, the changes in the data folder; the host merges them)
struct Checkpoint { float x, y, z, radius; };
struct Settings {
    bool show = true, mph = false;
    float x = 560, y = 240, scale = 1;
    int detailsKey = -1;            // held: more lines (fuel, damage, the position)
    Checkpoint start{}, finish{};
} S;

Checkpoint ReadCheckpoint(const char *name) {
    std::string k = name;
    return {static_cast<float>(H->config_number(M, (k + ".x").c_str(), 0)), static_cast<float>(H->config_number(M, (k + ".y").c_str(), 0)),
            static_cast<float>(H->config_number(M, (k + ".z").c_str(), 0)), static_cast<float>(H->config_number(M, (k + ".radius").c_str(), 8))};
}

// ---- the lap
enum LapState { kWaiting, kRunning, kFinished };   // waiting: for the car to leave the start circle
LapState g_lap = kWaiting;
float g_time, g_last, g_best;      // seconds; 0: none
bool g_paused, g_wasInStart;
std::string g_mission;             // the mission the best lap is for
int g_slotGas = -1, g_slotBrake = -1;

void LapStart() { g_lap = kRunning; g_time = 0; }
void LapReset() { g_lap = kWaiting; g_time = 0; }

// The hotkey: a lap starts here and now (again: it starts anew).
void OnLapKey(void *) {
    if (H->mission_state() != MML_MISSION_RUNNING || !game::PlayerCar()) return;
    LapStart();
    H->log(M, MML_LOG_INFO, "lap started by the hotkey");
}

// (Called again when the settings change: the same function and user replace the combination.)
void Bind() {
    const char *combo = H->config_string(M, "hotkey_lap", "F9");
    if (!H->hotkey(M, combo, OnLapKey, nullptr)) H->log(M, MML_LOG_WARN, "hotkey_lap '%s' is not a key combination", combo);
}

void ReadSettings() {
    S.show = H->config_number(M, "overlay.show", 1) != 0;
    S.x = static_cast<float>(H->config_number(M, "overlay.x", 560));
    S.y = static_cast<float>(H->config_number(M, "overlay.y", 240));
    S.scale = static_cast<float>(H->config_number(M, "overlay.scale", 1));
    if (!(S.scale > 0.2f && S.scale < 5.0f)) S.scale = 1;
    S.mph = !_stricmp(H->config_string(M, "units", "kmh"), "mph");
    S.detailsKey = H->key_code(H->config_string(M, "key_details", "F8"));
    S.start = ReadCheckpoint("start");
    S.finish = ReadCheckpoint("finish");
    Bind();
}

// ---- files: the best lap of each mission, <game>\mml\data\telemetry\best_<mission>.txt
std::string BestPath() { return std::string(H->mod_data_dir(M)) + "best_" + g_mission + ".txt"; }

void LoadBest() {
    g_best = 0;
    if (g_mission.empty()) return;
    if (FILE *f = fopen(BestPath().c_str(), "r")) {
        if (fscanf(f, "%f", &g_best) != 1 || !(g_best > 0)) g_best = 0;
        fclose(f);
    }
}

void SaveBest() {
    if (FILE *f = fopen(BestPath().c_str(), "w")) {
        fprintf(f, "%.3f\n", g_best);
        fclose(f);
    }
    // and every finished lap, one line each
    if (FILE *f = fopen((std::string(H->mod_data_dir(M)) + "laps.csv").c_str(), "a")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%s,%.3f\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, g_mission.c_str(), g_last);
        fclose(f);
    }
}

// ---- the mission's tick: the world moved by ev->dt seconds (never while the game is paused or loading)
bool Inside(const V3 &p, const Checkpoint &c) {
    float dx = p.x - c.x, dz = p.z - c.z;   // on the ground: y is up
    return dx * dx + dz * dz <= c.radius * c.radius;
}

void OnTick(const MmlEvent *ev, void *) {
    game::Car car(game::PlayerCar());   // the car the player sits in now; nothing on foot
    if (!car) return;
    V3 pos = car.Position();
    bool inStart = Inside(pos, S.start);
    // (only a car that was in the circle leaves it: a lap does not start somewhere else in the city)
    if (g_lap == kWaiting && g_wasInStart && !inStart) LapStart();
    g_wasInStart = inStart;
    if (g_lap == kRunning) {
        g_time += ev->dt;
        if (Inside(pos, S.finish)) {
            g_lap = kFinished;
            g_last = g_time;
            bool best = g_best <= 0 || g_last < g_best;
            if (best) g_best = g_last;
            SaveBest();
            H->log(M, MML_LOG_INFO, "lap %.3f s%s", g_last, best ? " (the best)" : "");
        }
    } else if (g_lap == kFinished && inStart) {
        LapReset();   // back at the start: ready for the next one
    }
}

// ---- drawing: queued every frame (MML_EVENT_FRAME comes in the menus and the pause too), in 800x600 units
void Time(char *out, size_t n, float s) {
    if (s <= 0) { snprintf(out, n, "--:--.---"); return; }
    int ms = static_cast<int>(s * 1000.0f + 0.5f);
    snprintf(out, n, "%02d:%02d.%03d", ms / 60000, ms / 1000 % 60, ms % 1000);
}

void Bar(float x, float y, float w, float h, float value, uint32_t argb) {
    value = value < 0 ? 0 : value > 1 ? 1 : value;
    H->draw_rect(M, x, y, w, h, 0x60000000u, 1);
    if (value > 0) H->draw_rect(M, x, y, w * value, h, argb, 1);
    H->draw_rect(M, x, y, w, h, 0xA0FFFFFFu, 0);
}

void Draw() {
    if (!S.show) return;
    int state = H->mission_state();
    if (state != MML_MISSION_RUNNING && state != MML_MISSION_PAUSED) return;   // no world: nothing to tell
    const float k = S.scale, x = S.x, y = S.y, w = 220 * k;
    const uint32_t white = 0xFFFFFFFFu, grey = 0xFFB0B0B0u, yellow = 0xFFFFD040u, green = 0xFF40E060u, red = 0xFFFF5040u;
    bool details = H->key_down(S.detailsKey) != 0;
    float h = (details ? 196 : 148) * k;
    H->draw_rect(M, x, y, w, h, 0xA0101010u, 1);
    H->draw_rect(M, x, y, w, h, 0xFF606060u, 0);
    char line[160];
    snprintf(line, sizeof line, "TELEMETRY  %s", g_mission.c_str());
    H->draw_text(M, x + 8 * k, y + 4 * k, 13 * k, grey, MML_FONT_SMALL, MML_DRAW_LEFT, line);
    H->draw_line(M, x + 6 * k, y + 22 * k, x + w - 6 * k, y + 22 * k, 0xFF606060u);

    game::Car car(game::PlayerCar());
    if (!car) {
        H->draw_text(M, x + w / 2, y + 44 * k, 18 * k, grey, MML_FONT_SMALL, MML_DRAW_CENTER, "on foot");
    } else {
        // the speed, big, its unit beside it; the gear on the right
        snprintf(line, sizeof line, "%.0f", S.mph ? car.Speed() * 2.2369363f : car.SpeedKmh());
        H->draw_text(M, x + 96 * k, y + 24 * k, 44 * k, white, MML_FONT_BIG, MML_DRAW_RIGHT, line);
        H->draw_text(M, x + 102 * k, y + 46 * k, 15 * k, grey, MML_FONT_SMALL, MML_DRAW_LEFT, S.mph ? "mph" : "km/h");
        int gear = car.Gear();
        snprintf(line, sizeof line, gear < 0 ? "R" : gear == 0 ? "N" : "%d", gear);
        H->draw_text(M, x + w - 12 * k, y + 24 * k, 44 * k, yellow, MML_FONT_BIG, MML_DRAW_RIGHT, line);
        // the engine's speed, and what the driver's feet do (the game's actions, whatever keys they are bound to;
        // each action has two slots: the primary and the secondary binding)
        Bar(x + 8 * k, y + 72 * k, w - 16 * k, 7 * k, car.Rpm() / 6000.0f, yellow);
        float gas = std::max(H->action_held(g_slotGas), H->action_held(g_slotGas + 1));
        float brake = std::max(H->action_held(g_slotBrake), H->action_held(g_slotBrake + 1));
        Bar(x + 8 * k, y + 83 * k, (w - 20 * k) / 2, 5 * k, gas, green);
        Bar(x + 12 * k + (w - 20 * k) / 2, y + 83 * k, (w - 20 * k) / 2, 5 * k, brake, red);
    }
    // the lap
    char t[32];
    Time(t, sizeof t, g_lap == kFinished ? g_last : g_time);
    snprintf(line, sizeof line, "LAP   %s", g_lap == kWaiting ? "--:--.---" : t);
    H->draw_text(M, x + 8 * k, y + 94 * k, 20 * k, g_lap == kFinished ? green : white, MML_FONT_BIG, MML_DRAW_LEFT, line);
    Time(t, sizeof t, g_best);
    snprintf(line, sizeof line, "BEST  %s", t);
    H->draw_text(M, x + 8 * k, y + 114 * k, 20 * k, yellow, MML_FONT_BIG, MML_DRAW_LEFT, line);
    const char *hint = g_paused ? "paused" : !car ? "get into a car" : g_lap == kWaiting ? "leave the start, or press the lap key"
                     : g_lap == kRunning ? "to the finish" : "finished: back to the start for another";
    H->draw_text(M, x + w / 2, y + 133 * k, 12 * k, grey, MML_FONT_SMALL, MML_DRAW_CENTER, hint);
    if (details && car) {
        V3 p = car.Position();
        snprintf(line, sizeof line, "fuel %.1f / %.0f l   damage %.0f%%", car.Fuel(), car.FuelTank(), car.Damage() * 100);
        H->draw_text(M, x + 8 * k, y + 150 * k, 12 * k, white, MML_FONT_SMALL, MML_DRAW_LEFT, line);
        snprintf(line, sizeof line, "rpm %.0f   steer %+.2f", car.Rpm(), car.Steering());
        H->draw_text(M, x + 8 * k, y + 164 * k, 12 * k, white, MML_FONT_SMALL, MML_DRAW_LEFT, line);
        snprintf(line, sizeof line, "pos %.1f  %.1f  %.1f", p.x, p.y, p.z);
        H->draw_text(M, x + 8 * k, y + 178 * k, 12 * k, white, MML_FONT_SMALL, MML_DRAW_LEFT, line);
    }
}

void OnFrame(const MmlEvent *, void *) { Draw(); }

// ---- the mission and the settings, as events
void OnMission(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_MISSION_OPEN:      // a new mission loads (ev->name): the old lap means nothing there
        LapReset();
        g_last = 0;
        break;
    case MML_EVENT_MISSION_LOADED:    // its world runs: the player and the cars exist from here on
        g_mission = ev->name;
        LoadBest();
        break;
    case MML_EVENT_GAME_PAUSED: g_paused = true; break;
    case MML_EVENT_GAME_RESUMED: g_paused = false; break;
    case MML_EVENT_CONFIG_CHANGED:    // config.json was edited, or config.set / config_set changed a setting
        ReadSettings();
        H->log(M, MML_LOG_INFO, "settings read again");
        break;
    default: break;
    }
}

// ---- the game's own menus: an item "Telemetry" in the pause menu (under Options), and a page of the mod's own
// that it opens, made by the game from these descriptions as it makes its Options page (its paper, fonts, keys,
// sounds). Ids rise in the order the keyboard walks them. menu.state on the bridge shows any open menu's controls.
enum { kItemAfterOptions = 108, kBest = 11, kLast = 13, kFuel = 15, kOverlay = 101, kUnits = 102, kBack = 400 };

// A control that changes calls this with its id (a click, Enter, a drag): its new value is read and kept as a setting
// (MML_EVENT_CONFIG_CHANGED brings it back to ReadSettings).
void OnPage(int id, void *) {
    float v = 0;
    switch (id) {
    case kOverlay:   // a check box: 0 or 1
        if (H->menu_value(M, kOverlay, &v)) H->config_set(M, "overlay.show", v != 0 ? "true" : "false");
        break;
    case kUnits:     // a switch between two texts: 0 the first, 1 the second
        if (H->menu_value(M, kUnits, &v)) H->config_set(M, "units", v != 0 ? "\"mph\"" : "\"kmh\"");
        break;
    case kBack: H->menu_close(M); break;
    default: break;
    }
}

void OpenPage() {
    char best[32], last[32];
    Time(best, sizeof best, g_best);
    Time(last, sizeof last, g_last);
    const MmlMenuControl page[] = {
        {1, "wind", 40, 180, 300, 230, "Telemetry:", 0},   // over the pause menu, where the game puts its Options
        {10, "stxt", 20, 10, 120, 18, "Best lap", 0},
        {kBest, "stxt", 150, 8, 130, 22, best, MML_MENU_FONT_BIG},
        {12, "stxt", 20, 35, 120, 18, "Last lap", 0},
        {kLast, "stxt", 150, 33, 130, 22, last, MML_MENU_FONT_BIG},
        {14, "stxt", 20, 60, 120, 18, "Fuel", 0},
        {kFuel, "pbar", 150, 63, 130, 12, nullptr, 0},     // the game's loading bar
        {16, "line", 10, 90, 280, 0, nullptr, 0},
        {17, "stxt", 20, 103, 120, 18, "Overlay", 0},
        {kOverlay, "chck", 150, 101, 20, 20, nullptr, 0},   // a check box
        {18, "stxt", 20, 128, 120, 18, "Units", 0},
        {kUnits, "bslb", 150, 126, 130, 20, "km/h|mph", 0}, // a switch: its two texts
        {kBack, "butt", 0, 155, 300, 20, "Back", 0},
    };
    if (!H->menu_open(M, "laps", page, sizeof page / sizeof page[0], OnPage, nullptr)) return;   // the log tells why
    // values given right after menu_open are applied when the game makes the page
    game::Car car(game::LastCar());
    H->menu_set_value(M, kFuel, car && car.FuelTank() > 0 ? car.Fuel() / car.FuelTank() : 0);
    H->menu_set_value(M, kOverlay, S.show ? 1.0f : 0.0f);
    H->menu_set_value(M, kUnits, S.mph ? 1.0f : 0.0f);
}

void OnMenuItem(int, void *) { OpenPage(); }

// ---- commands: what agents call over MCP (`call` with the method "telemetry.<name>"); the reply is JSON text
int Status(const char *, MmlReply *r, void *) {
    static const char *kLap[] = {"waiting", "running", "finished"};
    char out[512];
    game::Car car(H->mission_state() == MML_MISSION_NONE ? nullptr : game::PlayerCar());
    snprintf(out, sizeof out,
             "{\"mission\":\"%s\",\"in_car\":%s,\"speed_kmh\":%.2f,\"lap\":\"%s\",\"time\":%.3f,\"last\":%.3f,\"best\":%.3f,"
             "\"start\":[%.2f,%.2f,%.2f,%.1f],\"finish\":[%.2f,%.2f,%.2f,%.1f],\"units\":\"%s\",\"show\":%s}",
             g_mission.c_str(), car ? "true" : "false", car ? car.SpeedKmh() : 0.0f, kLap[g_lap], g_time, g_last, g_best,
             S.start.x, S.start.y, S.start.z, S.start.radius, S.finish.x, S.finish.y, S.finish.z, S.finish.radius,
             S.mph ? "mph" : "kmh", S.show ? "true" : "false");
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

// telemetry.lap {"do": "start" | "reset"}
int Lap(const char *args, MmlReply *r, void *) {
    if (strstr(args, "\"reset\"")) LapReset();
    else if (strstr(args, "\"start\"")) LapStart();
    else { H->reply_error(r, "do: \"start\" (a lap from here and now) or \"reset\""); return 1; }
    return Status(args, r, nullptr);
}

// telemetry.checkpoint {"which": "start" | "finish"}: the checkpoint is put where the car is (a setting is written:
// the change is in the data folder's config.json, and MML_EVENT_CONFIG_CHANGED brings it back to ReadSettings)
int SetCheckpoint(const char *args, MmlReply *r, void *) {
    const char *which = strstr(args, "\"finish\"") ? "finish" : strstr(args, "\"start\"") ? "start" : nullptr;
    game::Car car(H->mission_state() == MML_MISSION_NONE ? nullptr : game::PlayerCar());
    if (!which || !car) { H->reply_error(r, !which ? "which: \"start\" or \"finish\"" : "the player is not in a car"); return 1; }
    V3 p = car.Position();
    char value[160];
    snprintf(value, sizeof value, "{\"x\": %.2f, \"y\": %.2f, \"z\": %.2f}", p.x, p.y, p.z);
    if (!H->config_set(M, which, value)) { H->reply_error(r, "the setting could not be written"); return 1; }
    snprintf(value, sizeof value, "{\"%s\":[%.2f,%.2f,%.2f]}", which, p.x, p.y, p.z);
    H->reply_append(r, value, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    // The kit is in loaders from 2026-10 on; the game's addresses are known for the builds game_build.h lists.
    if (!MML_HAS(host, config_set)) { host->log(mod, MML_LOG_ERROR, "this loader has no mod kit (update MafiaModLoader.asi)"); return 1; }
    if (!game::Detect(host->module_base("LS3DF.dll")) || !game::Exe().vehPos) { host->log(mod, MML_LOG_ERROR, "the car is not known for this build of the game"); return 2; }
    ReadSettings();
    g_slotGas = host->action_slot("Accelerate");
    g_slotBrake = host->action_slot("Brake");
    // loaded (or hot-reloaded) in the middle of a mission: no event will tell, so ask
    if (host->mission_state() >= MML_MISSION_RUNNING) { g_mission = host->mission_name(); LoadBest(); }
    g_paused = host->mission_state() == MML_MISSION_PAUSED;
    // what must survive this mod's own reloads while the game runs
    struct { int lap; float time, last; } kept{};
    if (host->state_get(mod, "lap", &kept, sizeof kept) == sizeof kept) { g_lap = static_cast<LapState>(kept.lap); g_time = kept.time; g_last = kept.last; }

    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnTick, nullptr);
    for (MmlEventType t : {MML_EVENT_MISSION_OPEN, MML_EVENT_MISSION_LOADED, MML_EVENT_GAME_PAUSED, MML_EVENT_GAME_RESUMED, MML_EVENT_CONFIG_CHANGED})
        host->subscribe(mod, t, OnMission, nullptr);
    // the item is in the pause menu from its next opening (and after this mod's reloads: asked again, kept once)
    if (MML_HAS(host, menu_value) && !host->menu_item(mod, "gamemenu", kItemAfterOptions, "Telemetry", OnMenuItem, nullptr))
        host->log(mod, MML_LOG_WARN, "the game's menus are not known for this build: no item in the pause menu");
    host->command(mod, "status", "the lap, the best, the checkpoints, the car's speed", Status, nullptr);
    host->command(mod, "lap", "a lap started or reset (params: do = \"start\" | \"reset\")", Lap, nullptr);
    host->command(mod, "checkpoint", "a checkpoint put where the car is (params: which = \"start\" | \"finish\")", SetCheckpoint, nullptr);
    host->log(mod, MML_LOG_INFO, "telemetry: mission '%s', best %.3f s, data in %s", g_mission.c_str(), g_best, host->mod_data_dir(mod));
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *mod) {
    struct { int lap; float time, last; } kept{g_lap, g_time, g_last};
    H->state_put(mod, "lap", &kept, sizeof kept);
}
