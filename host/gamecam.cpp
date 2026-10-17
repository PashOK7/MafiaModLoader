// The game's own camera logic (mml.h game_camera, game_camera_mode, game_camera_fov; docs/MODDING.md "The game's
// camera"). After st0rm94's structs/camera.md, each field read in both builds' code (game_build.h gameCamera).
//
// G_Camera is the camera manager inside C_game: every frame its Tick places the I3D_camera it drives (+0x04) by its
// mode (+0x10), following the player (+0x08) or his car (+0x0C). A mode is switched by the game's own SetMode (End,
// the mode, Begin), as the camera-change key does; the game then drives the new mode as its own. Only the modes the
// player's state allows are offered: on foot behind him or from his eyes, in a vehicle the six its key cycles
// through; the others (the scope, a dialogue, a death, a lock, a race) belong to what sets them.
//
// The field of view is the I3D_camera's (+0x140, the near pass' +0x14C), set by its own setters (SetFOV +0x50,
// SetNearFOV +0x54); the game changes it only at a few moments (a mode pushed and popped, a dialogue's camera, the
// scope), so a held value is set again at the next frame whenever the camera has another, and the game's own value
// (kept at the first change) is given back when the holder lets go.
//
// Bridge method (game thread): camera.game {mode, fov (null: the game's back)}.
#include "host.h"
#include <cmath>
#include <cstring>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kCamI3D = 0x04, kCamCar = 0x0C, kCamMode = 0x10;
constexpr int kI3DFov = 0x140, kI3DNear = 0x144, kI3DFar = 0x148, kI3DNearFov = 0x14C;
constexpr int kVtSetFov = 0x50, kVtSetNearFov = 0x54;
constexpr int kRecord = 0x44, kRecordName = 0x30, kRecords = 17;

using SetModeFn = void *(__fastcall *)(void *camera, void *edx, unsigned mode, char begin);
using SetFloatFn = int(__stdcall *)(void *cam, float v);

MmlMod *g_fovOwner;        // the holder of the field of view (kLoaderOwner: the bridge's)
float g_fov;               // held
float g_gameFov, g_gameNearFov;   // the game's own, kept at the first change
bool g_haveGame;

template <class T> T &At(void *p, int off) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off); }

void *Manager() {
    if (!game::g_exe || !game::Exe().cameraSetMode) return nullptr;
    void *g = game::Game();
    return g ? static_cast<uint8_t *>(g) + game::Exe().gameCamera : nullptr;
}
void *I3D(void *m) { return m ? At<void *>(m, kCamI3D) : nullptr; }

bool OnFoot(int mode) { return mode == 1 || mode == 3; }
bool InCar(int mode) { return mode == 7 || mode == 8 || mode == 9 || mode == 12 || mode == 13 || mode == 14; }

void ModeName(int mode, char out[16]) {
    out[0] = 0;
    const uint8_t *t = reinterpret_cast<const uint8_t *>(game::Exe().cameraModeTable);
    for (int i = 0; i < kRecords; i++)
        if (*reinterpret_cast<const int *>(t + i * kRecord) == mode) {
            memcpy(out, t + i * kRecord + kRecordName, 15);
            out[15] = 0;
            return;
        }
}

bool Read(MmlGameCamera *out) {
    void *m = Manager();
    void *cam = I3D(m);
    if (!m || !cam) return false;
    out->mode = At<int>(m, kCamMode);
    ModeName(out->mode, out->mode_name);
    out->car = At<void *>(m, kCamCar);
    out->fov = At<float>(cam, kI3DFov);
    out->near_z = At<float>(cam, kI3DNear);
    out->far_z = At<float>(cam, kI3DFar);
    out->fov_held = g_fovOwner ? 1 : 0;
    return true;
}
DWORD SafeRead(MmlGameCamera *out, bool *ok) {
    __try { *ok = Read(out); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return GetExceptionCode(); }
}

bool SetMode(int mode, std::string *why) {
    void *m = Manager();
    if (!m || !I3D(m)) { *why = "no mission (no game camera)"; return false; }
    int now = At<int>(m, kCamMode);
    bool car = At<void *>(m, kCamCar) != nullptr;
    if (!(OnFoot(now) && OnFoot(mode) && !car) && !(InCar(now) && InCar(mode) && car)) {
        char a[16], b[16];
        ModeName(now, a);
        ModeName(mode, b);
        *why = "the camera is in mode " + std::to_string(now) + (a[0] ? std::string(" (") + a + ")" : "") + ": it can go to " +
               (InCar(now) ? "7, 8, 9, 12, 13, 14 (in a vehicle)" : OnFoot(now) ? "1, 3 (on foot)" : "none of the modes offered now (the game's own)");
        return false;
    }
    reinterpret_cast<SetModeFn>(game::Exe().cameraSetMode)(m, nullptr, static_cast<unsigned>(mode), 1);
    return true;
}
DWORD SafeSetMode(int mode, std::string *why, bool *ok) {
    __try { *ok = SetMode(mode, why); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return GetExceptionCode(); }
}

void PutFov(void *cam, float fov, float nearFov) {
    (*reinterpret_cast<SetFloatFn *>(*reinterpret_cast<uint8_t **>(cam) + kVtSetFov))(cam, fov);
    (*reinterpret_cast<SetFloatFn *>(*reinterpret_cast<uint8_t **>(cam) + kVtSetNearFov))(cam, nearFov);
}

bool SetFov(MmlMod *owner, float fov, std::string *why) {
    void *cam = I3D(Manager());
    if (fov == 0) {   // the game's back
        if (g_fovOwner && g_fovOwner != owner) { *why = "another mod holds the field of view"; return false; }
        if (g_fovOwner && cam && g_haveGame) PutFov(cam, g_gameFov, g_gameNearFov);
        g_fovOwner = nullptr;
        g_haveGame = false;
        return true;
    }
    if (!(fov >= 0.1f && fov <= 3.0f)) { *why = "a field of view of 0.1..3.0 radians (0: the game's)"; return false; }
    if (!cam) { *why = "no mission (no game camera)"; return false; }
    if (!g_haveGame) {
        g_gameFov = At<float>(cam, kI3DFov);
        g_gameNearFov = At<float>(cam, kI3DNearFov);
        g_haveGame = true;
    }
    g_fovOwner = owner;
    g_fov = fov;
    PutFov(cam, fov, fov);
    return true;
}
DWORD SafeSetFov(MmlMod *owner, float fov, std::string *why, bool *ok) {
    __try { *ok = SetFov(owner, fov, why); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return GetExceptionCode(); }
}

void Hold() {
    if (!g_fovOwner) return;
    void *cam = I3D(Manager());
    if (cam && (At<float>(cam, kI3DFov) != g_fov || At<float>(cam, kI3DNearFov) != g_fov)) PutFov(cam, g_fov, g_fov);
}
DWORD SafeHold() { __try { Hold(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); } }

} // namespace

int GameCamera(MmlGameCamera *out) {
    if (!out) return 0;
    MmlGameCamera c{};
    c.size = sizeof c;
    bool ok = false;
    if (SafeRead(&c, &ok) || !ok) return 0;
    uint32_t size = out->size && out->size < sizeof c ? out->size : static_cast<uint32_t>(sizeof c);
    memcpy(out, &c, size);
    out->size = size;
    return 1;
}

int GameCameraMode(MmlMod *mod, int mode) {
    std::string why;
    bool ok = false;
    if (SafeSetMode(mode, &why, &ok)) why = "a fault in the game's SetMode";
    if (!ok) Log(MML_LOG_WARN, ModIdOf(mod), "game_camera_mode %d: %s", mode, why.c_str());
    return ok ? 1 : 0;
}

int GameCameraFov(MmlMod *mod, float fov) {
    std::string why;
    bool ok = false;
    if (SafeSetFov(mod, fov, &why, &ok)) why = "a fault in the camera's setters";
    if (!ok) Log(MML_LOG_WARN, ModIdOf(mod), "game_camera_fov %g: %s", fov, why.c_str());
    return ok ? 1 : 0;
}

void GameCameraFrame() { if (g_fovOwner) SafeHold(); }   // (render.cpp: before the world's drawing)

void GameCameraModGone(MmlMod *mod) {
    if (g_fovOwner != mod) return;
    std::string why;
    bool ok = false;
    SafeSetFov(mod, 0, &why, &ok);
}

void GameCameraMissionClose() { g_fovOwner = nullptr; g_haveGame = false; }   // (the next mission's camera is the game's)

bool GameCameraMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "camera.game") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    std::string why;
    bool done = false;
    if (p.contains("mode")) {
        if (SafeSetMode(p["mode"].get<int>(), &why, &done)) why = "a fault in the game's SetMode";
        if (!done) { *reply = "camera.game mode: " + why; return true; }
    }
    if (p.contains("fov")) {
        float fov = p["fov"].is_number() ? p["fov"].get<float>() : 0.0f;
        if (SafeSetFov(kLoaderOwner, fov, &why, &done)) why = "a fault in the camera's setters";
        if (!done) { *reply = "camera.game fov: " + why; return true; }
    }
    MmlGameCamera c{};
    c.size = sizeof c;
    if (!GameCamera(&c)) { *reply = "camera.game: no mission (no game camera)"; return true; }
    char car[16];
    snprintf(car, sizeof car, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(c.car)));
    *reply = json{{"mode", c.mode}, {"mode_name", c.mode_name}, {"car", c.car ? json(car) : json(nullptr)}, {"fov", c.fov},
                  {"fov_degrees", std::round(c.fov * 57.29578f * 10) / 10}, {"near", c.near_z}, {"far", c.far_z},
                  {"fov_held_by", g_fovOwner ? (g_fovOwner == kLoaderOwner ? "the bridge" : ModIdOf(g_fovOwner)) : "none"},
                  {"modes_now", InCar(c.mode) ? json::array({7, 8, 9, 12, 13, 14}) : OnFoot(c.mode) && !c.car ? json::array({1, 3}) : json::array()}}
                 .dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
