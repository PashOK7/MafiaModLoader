// devtools: the developer's switches, for whoever writes missions and mods (both builds): the player kept from harm,
// a weapon that never runs dry, flying, a free camera, the field of view, the game's HUD hidden. A port of pet1kk's
// Developer Tools (MafiaModLoader PR 2, a standalone ASI for 1.2) onto the loader: each switch is the SDK's, or the
// game's own function hooked through the loader, and works in 1.2 and 1.0.
//
// - god (human_invulnerable): the game's harm (C_human::Hit: bullets, blows, explosions, cars, falls) not run for him.
// - infinite_ammo: the weapon in his hand filled to its magazine and its usual reserve every tick (item_ammo_set,
//   the sizes from the game's items table).
// - fly (F2): he moves where the camera looks, Forwards, Backwards, Left, Right as on foot, Jump up, Crouch down,
//   Walk/Run three times as fast, the mouse wheel's Cycle Up / Cycle Down the speed; every binding of each action
//   (the controls' primary and secondary rows: action_held gives the action whichever is used). Not in a vehicle. The game's own movement
//   (C_human::MovementIntern: gravity, the ground, the fall) and jump (C_human::Do_Jump) are not run for him while he
//   flies (hooked: game_build.h humanMove, humanJump); his place is set after each tick of the mission as the scripts'
//   ACTOR_SETPOS sets it (the actor's place, its frame's, the sector it is in), so the walk plays where he floats.
// - freecam (F3): the frame drawn from a camera of the mod's (camera_override): the mouse turns it as it turns the
//   player (the options' sensitivity; Aim X, Aim Y kept from the game: the player stands), the same keys move it;
//   the game's camera, the aim and the sound stay.
// - fov: the field of view the game draws with (game_camera_fov), 0 the game's own.
// - hide_hud (F4): the game's HUD not drawn (hud_game); the menus, the console and the mods' drawing stay.
//
// Settings: config.json, the game's Options > Mods > Developer tools. Command: devtools.state.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool g_game;   // a build whose addresses are known (game_build.h)

struct V { float x, y, z; };
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Len(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V Norm(V a) { float l = Len(a); return l > 1e-6f ? a * (1 / l) : a; }

struct Settings {
    bool god = false, ammo = false, fly = false, freecam = false, hideHud = false;
    float flySpeed = 10, camSpeed = 10, fov = 0, mouse = 1;
} S;

template <class T> T R(uintptr_t a) {
    T v{};
    if (a) H->mem_read(a, &v, sizeof v);
    return v;
}
void *Player() { return H->human_player(M); }
bool PlayerPos(V *out) {
    void *p = Player();
    float w[16];
    uintptr_t frame = p ? R<uintptr_t>(reinterpret_cast<uintptr_t>(p) + game::kActorFrame) : 0;
    if (!frame || !H->frame_world(M, reinterpret_cast<void *>(frame), w)) return false;
    *out = {w[12], w[13], w[14]};
    return true;
}
// In any vehicle (a car, a tram, a boat: C_human +0x98), not only a car: flying would pull him out of it.
bool InVehicle(void *p) { return p && game::at<void *>(p, game::kHumanVehicle) != nullptr; }
// The game's own turn for a mouse axis' value: the options' sensitivity * 5 radians (C_player's on-foot input,
// game_build.h mouseSensitivity), so the free camera turns as the player does; 0.49 when the build's is not known.
float MouseTurn() {
    float sens = 0;
    if (g_game && game::Exe().mouseSensitivity) H->mem_read(game::Exe().mouseSensitivity, &sens, sizeof sens);
    if (!(sens > 0.01f && sens < 10)) sens = 0.49f;
    return sens * 5;
}

// ---- the game's actions, as the player gives them
int g_aForwards, g_aBackwards, g_aLeft, g_aRight, g_aJump, g_aCrouch, g_aRun, g_aAimX, g_aAimY, g_aCycleUp, g_aCycleDown;
float Held(int slot) { return slot >= 0 ? H->action_held(slot) : 0; }
float Pressed(int slot) { return slot >= 0 ? H->action_pressed(slot) : 0; }
void Capture(int slot, bool on) { if (slot >= 0) H->action_capture(M, slot, on ? 1 : 0); }

// The move the keys ask for: forward along `fwd`, aside along the horizontal right, up and down the world's.
V Wanted(V fwd, float *fast) {
    V right = Norm(V{fwd.z, 0, -fwd.x});   // (left-handed, y up: right = up x forward)
    V m = fwd * (Held(g_aForwards) - Held(g_aBackwards)) + right * (Held(g_aRight) - Held(g_aLeft));
    m.y += Held(g_aJump) - Held(g_aCrouch);
    *fast = Held(g_aRun) > 0.5f ? 3.0f : 1.0f;
    float l = Len(m);
    return l > 1 ? m * (1 / l) : m;
}
void SpeedKeys(float *speed) {   // the wheel (Cycle Up, Cycle Down): a quarter faster or slower a notch
    if (Pressed(g_aCycleUp) > 0) *speed = (std::min)(*speed * 1.25f, 300.0f);
    if (Pressed(g_aCycleDown) > 0) *speed = (std::max)(*speed / 1.25f, 1.0f);
}

// ---- flying
using MoveFn = void(__fastcall *)(void *human, void *edx, unsigned ms);
using JumpFn = bool(__fastcall *)(void *human, void *edx);
MoveFn oMove;
JumpFn oJump;
bool g_hooked;   // both hooked (a hook in place calls the game's through its own original)
bool g_flying;
V g_flyPos;
DWORD g_linkedAt;
void __fastcall hkMove(void *human, void *edx, unsigned ms) {
    if (g_flying && human == Player()) {   // (no gravity, no ground, no fall: the place is the mod's)
        game::at<float>(human, game::kHumanFallSpeed) = 0;
        if (int st = game::Exe().humanMoveState; st && game::at<int>(human, st) == game::kHumanStateAir) game::at<int>(human, st) = 1;   // (on his feet)
        return;
    }
    oMove(human, edx, ms);
}
bool __fastcall hkJump(void *human, void *edx) {
    if (g_flying && human == Player()) return false;   // (Jump is "up" while he flies)
    return oJump(human, edx);
}

// His place set as the scripts' ACTOR_SETPOS sets it (game_build.h actorSetPos, frameSetPos, sceneMoved): the actor's
// own, its frame's, and the sector the frame is in (twice a second: the lights and the visibility follow it).
using ActorSetPosFn = void(__fastcall *)(void *actor, void *edx, float x, float y, float z);
using FrameSetPosFn = void(__fastcall *)(void *frame, void *edx, const V *pos);
using FrameUpdateFn = void(__stdcall *)(void *frame);
using SceneMovedFn = void(__fastcall *)(void *scene, void *edx, void *frame);
void PlaceAt(void *p, V at) {
    const game::ExeBuild &e = game::Exe();
    void *frame = game::at<void *>(p, game::kActorFrame);
    if (!frame || !e.actorSetPos || !e.frameSetPos) return;
    reinterpret_cast<ActorSetPosFn>(e.actorSetPos)(p, nullptr, at.x, at.y, at.z);
    reinterpret_cast<FrameSetPosFn>(e.frameSetPos)(frame, nullptr, &at);
    reinterpret_cast<FrameUpdateFn>((*reinterpret_cast<void ***>(frame))[0x18 / 4])(frame);
    if (e.sceneMoved && GetTickCount() - g_linkedAt > 500) {
        g_linkedAt = GetTickCount();
        if (void *m = game::Mission()) reinterpret_cast<SceneMovedFn>(e.sceneMoved)(game::at<void *>(m, game::kMissionScene), nullptr, frame);
    }
}

void FlyTick(float dt) {
    void *p = Player();
    const bool want = S.fly && g_hooked && p && !InVehicle(p) && !S.freecam;
    if (want != g_flying) {
        g_flying = want;
        if (want && !PlayerPos(&g_flyPos)) g_flying = false;
        Capture(g_aJump, g_flying);
        Capture(g_aCrouch, g_flying);
        Capture(g_aCycleUp, g_flying);
        Capture(g_aCycleDown, g_flying);
        H->log(M, MML_LOG_INFO, "flying %s", g_flying ? "on" : "off");
    }
    if (!g_flying) return;
    float o[3], d[3];
    V fwd{0, 0, 1};
    if (H->camera_ray(M, 400, 300, o, d)) fwd = Norm(V{d[0], d[1], d[2]});
    SpeedKeys(&S.flySpeed);
    float fast = 1;
    V m = Wanted(fwd, &fast);
    g_flyPos = g_flyPos + m * (S.flySpeed * fast * (std::min)(dt, 0.1f));
    PlaceAt(p, g_flyPos);
}

// ---- the free camera
V g_camPos;
float g_yaw, g_pitch;
bool g_camOn;
DWORD g_camLast;
int PlaceCamera(MmlViewCamera *c, void *) {
    if (!g_camOn) return 0;
    const float cp = std::cos(g_pitch);
    V fwd{std::sin(g_yaw) * cp, -std::sin(g_pitch), std::cos(g_yaw) * cp};
    for (int i = 0; i < 3; i++) {
        c->world[8 + i] = (&fwd.x)[i];
        c->world[12 + i] = (&g_camPos.x)[i];
    }
    c->world[4] = 0; c->world[5] = 1; c->world[6] = 0;   // (the host makes right and up square to forward)
    c->flags |= MML_VIEW_OWN_SECTOR;
    return 1;
}
void CameraTick() {
    const int state = H->mission_state();   // (kept under the pause menu: the camera stays where it was)
    const bool want = S.freecam && g_game && (state == MML_MISSION_RUNNING || state == MML_MISSION_PAUSED);
    if (want != g_camOn) {
        g_camOn = want;
        if (want) {   // from where the game's camera is, looking where it looks
            float o[3], d[3];
            if (!H->camera_ray(M, 400, 300, o, d)) { g_camOn = false; return; }
            g_camPos = {o[0], o[1], o[2]};
            g_yaw = std::atan2(d[0], d[2]);
            g_pitch = -std::asin((std::max)(-1.0f, (std::min)(1.0f, d[1])));
            g_camLast = GetTickCount();
        }
        H->camera_override(M, want ? PlaceCamera : nullptr, nullptr);
        for (int a : {g_aForwards, g_aBackwards, g_aLeft, g_aRight, g_aJump, g_aCrouch, g_aRun, g_aAimX, g_aAimY, g_aCycleUp, g_aCycleDown}) Capture(a, want);
        H->log(M, MML_LOG_INFO, "free camera %s", want ? "on" : "off");
    }
    if (!g_camOn) return;
    DWORD now = GetTickCount();
    float dt = (std::min)((now - g_camLast) / 1000.0f, 0.1f);
    g_camLast = now;
    const float turn = MouseTurn() * S.mouse;
    g_yaw += Held(g_aAimX) * turn;
    g_pitch = (std::max)(-1.5f, (std::min)(1.5f, g_pitch + Held(g_aAimY) * turn));
    const float cp = std::cos(g_pitch);
    V fwd{std::sin(g_yaw) * cp, -std::sin(g_pitch), std::cos(g_yaw) * cp};
    SpeedKeys(&S.camSpeed);
    float fast = 1;
    V m = Wanted(fwd, &fast);
    g_camPos = g_camPos + m * (S.camSpeed * fast * dt);
}

// ---- the player kept from harm, his weapon kept full
void *g_godFor;   // the player held invulnerable (a new mission makes a new player)
void GodTick() {
    void *p = Player();
    if (g_godFor && (g_godFor != p || !S.god)) {
        if (g_godFor == p) H->human_invulnerable(M, p, 0);
        g_godFor = nullptr;
    }
    if (S.god && p && !g_godFor && H->human_invulnerable(M, p, 1) == 1) g_godFor = p;
}
void AmmoTick() {
    void *p = Player();
    if (!S.ammo || !p || !g_game || !game::Exe().items) return;
    MmlItem items[MML_SLOT_COUNT];
    const int n = H->human_items(M, p, items, MML_SLOT_COUNT);
    const uintptr_t table = R<uintptr_t>(game::Exe().items);
    const uint32_t count = R<uint32_t>(game::Exe().items + 4);
    for (int i = 0; i < n && i < MML_SLOT_COUNT; i++) {
        const MmlItem &it = items[i];
        if (it.slot != MML_SLOT_HAND || it.id <= 0 || static_cast<uint32_t>(it.id) >= count || !table) continue;
        const uintptr_t rec = table + static_cast<uintptr_t>(it.id) * game::kItemSize;
        const int magazine = R<int>(rec + game::kItemMagazine), reserve = R<int>(rec + game::kItemReserve);
        if (magazine <= 0 || magazine > 10000) continue;   // (not a weapon that is loaded)
        const int loaded = it.loaded < magazine ? magazine : -1, keep = reserve > 0 && it.reserve < reserve ? reserve : -1;
        if (loaded >= 0 || keep >= 0) H->item_ammo_set(M, p, MML_SLOT_HAND, loaded, keep);
    }
}

// ---- the settings
void Toggle(const char *key, bool *v) {
    *v = !*v;
    H->config_set(M, key, *v ? "true" : "false");
}
void ReadSettings() {
    S.god = H->config_number(M, "god", 0) != 0;
    S.ammo = H->config_number(M, "infinite_ammo", 0) != 0;
    S.fly = H->config_number(M, "fly", 0) != 0;
    S.freecam = H->config_number(M, "freecam", 0) != 0;
    S.hideHud = H->config_number(M, "hide_hud", 0) != 0;
    S.flySpeed = static_cast<float>(H->config_number(M, "fly_speed", 10));
    S.camSpeed = static_cast<float>(H->config_number(M, "freecam_speed", 10));
    S.mouse = static_cast<float>(H->config_number(M, "mouse", 1));
    S.fov = static_cast<float>(H->config_number(M, "fov", 0));
    H->hotkey(M, H->config_string(M, "fly_hotkey", "F2"), [](void *) { Toggle("fly", &S.fly); }, nullptr);
    H->hotkey(M, H->config_string(M, "freecam_hotkey", "F3"), [](void *) { Toggle("freecam", &S.freecam); }, nullptr);
    H->hotkey(M, H->config_string(M, "hud_hotkey", "F4"), [](void *) { Toggle("hide_hud", &S.hideHud); }, nullptr);
    H->hud_game(M, S.hideHud ? 0 : 1);
    H->game_camera_fov(M, S.fov >= 20 ? S.fov * 0.0174533f : 0);
}

void OnTick(const MmlEvent *ev, void *) {
    GodTick();
    AmmoTick();
    FlyTick(ev->dt);
}
void OnFrame(const MmlEvent *, void *) {
    CameraTick();
    const int st = H->mission_state();   // (a line of the HUD's layer: the pause menu is drawn over it)
    if ((st != MML_MISSION_RUNNING && st != MML_MISSION_PAUSED) || (!g_flying && !g_camOn)) return;
    char line[96];
    snprintf(line, sizeof line, g_camOn ? "free camera  %.0f m/s" : "flying  %.0f m/s", g_camOn ? S.camSpeed : S.flySpeed);
    H->draw_text(M, 400, 560, 14, 0xE0FFD040u, MML_FONT_SMALL, MML_DRAW_CENTER, line);
}
void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_CLOSE) {   // (the next mission's player is a new one)
        g_godFor = nullptr;
        g_flying = false;
    }
}

int State(const char *, MmlReply *reply, void *) {
    V p{};
    const bool have = PlayerPos(&p);
    char b[512];
    snprintf(b, sizeof b,
             "{\"god\":%s,\"invulnerable\":%s,\"infinite_ammo\":%s,\"flying\":%s,\"fly_speed\":%.1f,\"freecam\":%s,"
             "\"camera\":[%.2f,%.2f,%.2f],\"hud_shown\":%s,\"fov\":%.0f,\"hooks\":%s,\"player\":%s}",
             S.god ? "true" : "false", g_godFor ? "true" : "false", S.ammo ? "true" : "false", g_flying ? "true" : "false", S.flySpeed,
             g_camOn ? "true" : "false", g_camPos.x, g_camPos.y, g_camPos.z, H->hud_game(M, -1) == 1 ? "true" : "false", S.fov,
             g_hooked ? "true" : "false", have ? "true" : "false");
    H->reply_append(reply, b, strlen(b));
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, hud_game) || !MML_HAS(host, material_name)) return 1;   // a loader older than this mod
    g_game = game::Detect(host->module_base("LS3DF.dll"));
    if (!g_game) host->log(mod, MML_LOG_WARN, "not a known build of the game: flying is off");
    g_aForwards = host->action_slot("Forwards");
    g_aBackwards = host->action_slot("Backwards");
    g_aLeft = host->action_slot("Left");
    g_aRight = host->action_slot("Right");
    g_aJump = host->action_slot("Jump");
    g_aCrouch = host->action_slot("Crouch");
    g_aRun = host->action_slot("Walk/Run");
    g_aAimX = host->action_slot("Aim X");
    g_aAimY = host->action_slot("Aim Y");
    g_aCycleUp = host->action_slot("Cycle Up");
    g_aCycleDown = host->action_slot("Cycle Down");
    if (g_game && game::Exe().humanMove && game::Exe().humanJump) {
        const bool move = host->hook(mod, game::Exe().humanMove, reinterpret_cast<void *>(&hkMove), reinterpret_cast<void **>(&oMove)) != 0;
        const bool jump = host->hook(mod, game::Exe().humanJump, reinterpret_cast<void *>(&hkJump), reinterpret_cast<void **>(&oJump)) != 0;
        g_hooked = move && jump;
        if (!g_hooked) host->log(mod, MML_LOG_WARN, "the movement or the jump not hooked: flying is off");
    }
    ReadSettings();
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnTick, nullptr);
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->command(mod, "state", "the switches and what they do now: god, infinite_ammo, flying, freecam, the HUD, fov", State, nullptr);
    host->log(mod, MML_LOG_INFO, "devtools loaded (game %s)", host->game_build());
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    // (the loader takes back what is the mod's: its hooks, captures, camera, field of view, the HUD and the
    // invulnerable player)
}
