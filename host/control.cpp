// Control handed to mods (mml.h player_lock, vehicle_hook, vehicle_body; key_capture and action_capture are
// input.cpp's; docs/MODDING.md "Control").
//
// The player's own control: C_player::LockControls(bool), what the scripts' PLAYER_LOCKCONTROLS calls (then
// C_human::Do_Breath, as it does: a person who stood mid-step is set to stand). Locked, he stands where he is and the
// game's walking, shooting and driving leave him alone; the controls are still read (action_held), so a mod moves him
// itself (a skateboard, a ladder). The loader keeps who locked: the lock is let go at that mod's unload and when the
// mission closes.
//
// A car's physics: C_Vehicle::Engine (game_build.h vehicleEngine, thiscall(vehicle, float dt, f1, f2), ret 0xC), the
// step every car makes each frame, cut into Move's substeps. One mod's function (vehicle_hook) is told of each car's
// step with its body (position, the axes, the velocities); returning 1 the game's step is skipped and the body the
// mod wrote is the car's (the game stores the step's time, as Engine does; the model frame follows the body as it
// follows Move's). A plane, a hovercraft, a boat: the game's car with a mod's flight. vehicle_body reads and writes a
// car's body at any time.
//
// Bridge methods (game thread): player.lock {on}, vehicle.body {car, pos, velocity, turn, forward, up}, control.state.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

const game::ExeBuild &E() { return game::Exe(); }
bool LockKnown() { return game::g_exe && E().playerLockControls && E().humanDoBreath; }
bool BodyKnown() { return game::g_exe && E().vehicle && E().vehPos && E().vehAxes && E().vehVelocity && E().vehTurn && E().vehMatrix && E().vehMatrixPrev; }

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

using LockFn = void(__fastcall *)(void *player, void *edx, bool lock);   // thiscall, ret 4
using BreathFn = void(__fastcall *)(void *human, void *edx);
using EngineFn = void(__fastcall *)(void *vehicle, void *edx, float dt, float f1, float f2);   // thiscall, ret 0xC

MmlMod *g_lockedBy;
bool g_locked, g_lockedByBridge;
void *g_lockedPlayer;

struct VHook { MmlMod *mod; MmlVehicleFn fn; void *user; };
VHook g_vhook;
EngineFn oEngine;
bool g_engineHooked;
std::string g_engineWhy = "not asked for";
unsigned g_steps, g_skipped, g_vfaults;

bool IsCar(void *car) {
    bool ok = false;
    auto f = [&] { ok = car && game::at<int>(car, game::kActorType) == 4; };
    Guarded(f);
    return ok;
}

void ReadBody(void *car, MmlVehicleBody *b) {
    const game::ExeBuild &x = E();
    uint8_t *v = static_cast<uint8_t *>(car) + x.vehicle;
    V3 p = game::at<V3>(v, x.vehPos), vel = game::at<V3>(v, x.vehVelocity), w = game::at<V3>(v, x.vehTurn);
    const V3 *ax = &game::at<V3>(v, x.vehAxes);
    b->pos[0] = p.x; b->pos[1] = p.y; b->pos[2] = p.z;
    b->velocity[0] = vel.x; b->velocity[1] = vel.y; b->velocity[2] = vel.z;
    b->turn[0] = w.x; b->turn[1] = w.y; b->turn[2] = w.z;
    for (int i = 0; i < 3; i++) { b->axes[i][0] = ax[i].x; b->axes[i][1] = ax[i].y; b->axes[i][2] = ax[i].z; }
}
void WriteBody(void *car, const MmlVehicleBody *b) {
    const game::ExeBuild &x = E();
    uint8_t *v = static_cast<uint8_t *>(car) + x.vehicle;
    game::at<V3>(v, x.vehPos) = {b->pos[0], b->pos[1], b->pos[2]};
    game::at<V3>(v, x.vehVelocity) = {b->velocity[0], b->velocity[1], b->velocity[2]};
    game::at<V3>(v, x.vehTurn) = {b->turn[0], b->turn[1], b->turn[2]};
    V3 *ax = &game::at<V3>(v, x.vehAxes);
    for (int i = 0; i < 3; i++) ax[i] = {b->axes[i][0], b->axes[i][1], b->axes[i][2]};
}
bool Finite(const MmlVehicleBody &b) {
    const float *f = b.pos;
    for (int i = 0; i < 3; i++) if (!std::isfinite(b.pos[i]) || !std::isfinite(b.velocity[i]) || !std::isfinite(b.turn[i])) return false;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) if (!std::isfinite(b.axes[i][k])) return false;
    (void)f;
    return true;
}

// The body's matrices (game_build.h vehMatrix: A, the body now, = the model frame's world matrix: rows right, up,
// forward, the frame's place; +0x40 its inverse; vehMatrixPrev and +0x40 the step before's: Engine copies A into B
// first; 1.2 C_Vehicle +0xCE8 and +0xDE8, 1.0 +0xD64 and +0x68) and the
// model frame itself, which Move places (its local position and rotation, then its world matrix) each substep: a
// step a mod made does the same. The frame's place is the body's position plus the frame's offset from it in the
// body's axes, kept from before the step.
constexpr int kMatInv = 0x40;   // (each matrix's inverse after it)
using SetPosFn = void(__fastcall *)(void *frame, void *edx, const V3 *pos);
using UpdateFn = int(__stdcall *)(void *frame);

struct FrameOffset { V3 local; bool ok; };
FrameOffset OffsetOf(void *car, const MmlVehicleBody &b) {
    FrameOffset o{{0, 0, 0}, false};
    uint8_t *v = static_cast<uint8_t *>(car) + E().vehicle;
    const float *a = &game::at<float>(v, E().vehMatrix);
    V3 d{a[12] - b.pos[0], a[13] - b.pos[1], a[14] - b.pos[2]};
    V3 fwd{b.axes[0][0], b.axes[0][1], b.axes[0][2]}, right{b.axes[1][0], b.axes[1][1], b.axes[1][2]}, up{b.axes[2][0], b.axes[2][1], b.axes[2][2]};
    o.local = {Dot(d, right), Dot(d, up), Dot(d, fwd)};
    o.ok = Len(o.local) < 10.0f;
    return o;
}
void PlaceFrame(void *car, const MmlVehicleBody &b, const FrameOffset &o) {
    uint8_t *v = static_cast<uint8_t *>(car) + E().vehicle;
    V3 fwd = Norm({b.axes[0][0], b.axes[0][1], b.axes[0][2]}, {0, 0, 1}), right = Norm({b.axes[1][0], b.axes[1][1], b.axes[1][2]}, {1, 0, 0}),
       up = Norm({b.axes[2][0], b.axes[2][1], b.axes[2][2]});
    V3 p = V3{b.pos[0], b.pos[1], b.pos[2]} + (o.ok ? right * o.local.x + up * o.local.y + fwd * o.local.z : V3{0, 0, 0});
    float *a = &game::at<float>(v, E().vehMatrix), *ai = &game::at<float>(v, E().vehMatrix + kMatInv);
    memcpy(&game::at<float>(v, E().vehMatrixPrev), a, 64);
    memcpy(&game::at<float>(v, E().vehMatrixPrev + kMatInv), ai, 64);
    const V3 rows[3] = {right, up, fwd};
    for (int i = 0; i < 3; i++) { a[i * 4] = rows[i].x; a[i * 4 + 1] = rows[i].y; a[i * 4 + 2] = rows[i].z; a[i * 4 + 3] = 0; }
    a[12] = p.x; a[13] = p.y; a[14] = p.z; a[15] = 1;
    const float r[3][3] = {{right.x, right.y, right.z}, {up.x, up.y, up.z}, {fwd.x, fwd.y, fwd.z}};
    for (int i = 0; i < 3; i++) { ai[i * 4] = r[0][i]; ai[i * 4 + 1] = r[1][i]; ai[i * 4 + 2] = r[2][i]; ai[i * 4 + 3] = 0; }   // (the transpose)
    ai[12] = -Dot(p, right); ai[13] = -Dot(p, up); ai[14] = -Dot(p, fwd); ai[15] = 1;
    void *frame = game::at<void *>(car, game::kActorFrame);
    if (!frame) return;
    game::SetLocalRotation(frame, QuatOf(M3{{right, up, fwd}}));
    reinterpret_cast<SetPosFn>(E().frameSetPos)(frame, nullptr, &p);
    reinterpret_cast<UpdateFn>((*reinterpret_cast<void ***>(frame))[0x18 / 4])(frame);
}

struct StepCall { MmlVehicleStep *s; int r; };
void CallStep(void *p) {
    auto *c = static_cast<StepCall *>(p);
    c->r = g_vhook.fn(c->s, g_vhook.user);
}

void __fastcall hkEngine(void *vehicle, void *edx, float dt, float f1, float f2) {
    g_steps++;
    if (g_vhook.fn && ModAlive(g_vhook.mod)) {
        void *car = static_cast<uint8_t *>(vehicle) - E().vehicle;
        MmlVehicleStep s{};
        s.size = sizeof s;
        s.car = car;
        s.dt = dt;
        s.player = car == game::PlayerCar();
        bool read = false;
        FrameOffset off{};
        auto r = [&] { ReadBody(car, &s.body); off = OffsetOf(car, s.body); read = true; };
        Guarded(r);
        if (read) {
            StepCall c{&s, 0};
            if (!ModCall(g_vhook.mod, CallStep, &c, "its vehicle hook")) g_vfaults++;
            else if (c.r && Finite(s.body)) {
                auto w = [&] {
                    WriteBody(car, &s.body);
                    PlaceFrame(car, s.body, off);
                    // (kept awake: a car the mod held still would be let sleep by Tick, and a sleeping car stays
                    // where it is when the game's physics takes it back: seen floating until a pedal woke it)
                    // Tick's rest counters (both builds: +4 the ms within 0.15 m of the place at +8, flags +4 the ms
                    // calm; 4000 and 1000 ms put the car to sleep) started again, as Tick does when the car moves.
                    uint32_t &fl = game::at<uint32_t>(vehicle, E().vehicleFlags);
                    fl = (fl | game::kVehAwake) & ~game::kVehCalm;
                    game::at<int>(vehicle, 4) = 0;
                    game::at<V3>(vehicle, 8) = {s.body.pos[0], s.body.pos[1], s.body.pos[2]};
                    game::at<int>(vehicle, E().vehicleFlags + 4) = 0;
                    game::at<float>(vehicle, E().vehicleStepTime) = dt;   // (as Engine stores it: the ramps read it)
                };
                if (!Guarded(w)) { g_skipped++; return; }
            }
        }
    }
    oEngine(vehicle, edx, dt, f1, f2);
}

bool EnsureEngineHook(std::string *err) {
    if (g_engineHooked) return true;
    if (!BodyKnown() || !E().vehicleEngine || !E().vehicleStepTime) { *err = g_engineWhy = "this build's vehicle step is not known"; return false; }
    if (!HookAdd(nullptr, E().vehicleEngine, reinterpret_cast<void *>(&hkEngine), reinterpret_cast<void **>(&oEngine), err)) {
        g_engineWhy = *err;
        return false;
    }
    g_engineHooked = true;
    g_engineWhy = "hooked";
    return true;
}

bool SetLock(bool on) {
    void *pl = game::g_exe ? game::Player() : nullptr;
    if (!pl || !LockKnown()) return false;
    auto f = [&] {
        reinterpret_cast<LockFn>(E().playerLockControls)(pl, nullptr, on);
        reinterpret_cast<BreathFn>(E().humanDoBreath)(pl, nullptr);
    };
    if (Guarded(f)) return false;
    g_locked = on;
    g_lockedPlayer = on ? pl : nullptr;
    return true;
}

} // namespace

int PlayerLock(MmlMod *mod, int on) {
    if (!LockKnown()) return 0;
    if (!SetLock(on != 0)) return 0;
    g_lockedBy = on ? mod : nullptr;
    g_lockedByBridge = on && !mod;
    return 1;
}

int VehicleHook(MmlMod *mod, MmlVehicleFn fn, void *user) {
    if (!fn) { if (g_vhook.mod == mod) g_vhook = VHook{}; return 1; }
    std::string err;
    if (!EnsureEngineHook(&err)) { Log(MML_LOG_WARN, ModIdOf(mod), "vehicle_hook: %s", err.c_str()); return 0; }
    if (g_vhook.fn && g_vhook.mod != mod) Log(MML_LOG_WARN, ModIdOf(mod), "vehicle_hook: replaces %s's", ModIdOf(g_vhook.mod));
    g_vhook = VHook{mod, fn, user};
    return 1;
}

int VehicleBody(void *car, MmlVehicleBody *read, const MmlVehicleBody *write) {
    if (!BodyKnown() || !IsCar(car)) return 0;
    if (write && !Finite(*write)) return 0;
    auto f = [&] {
        if (write) WriteBody(car, write);
        if (read) ReadBody(car, read);
    };
    return Guarded(f) ? 0 : 1;
}

void ControlModGone(MmlMod *mod) {
    if (g_vhook.mod == mod) g_vhook = VHook{};
    if (mod && g_lockedBy == mod && g_locked) { SetLock(false); g_lockedBy = nullptr; }
}
void ControlMissionClose() {   // (the player goes with the mission: the next one's starts free)
    g_locked = false;
    g_lockedBy = nullptr;
    g_lockedByBridge = false;
    g_lockedPlayer = nullptr;
}

bool ControlMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "player.lock" && method != "vehicle.body" && method != "control.state") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    auto v3 = [](const float *v) { return json::array({std::round(v[0] * 1000) / 1000, std::round(v[1] * 1000) / 1000, std::round(v[2] * 1000) / 1000}); };
    try {
        if (method == "control.state") {
            *reply = dump({{"lock_known", LockKnown()}, {"locked", g_locked}, {"locked_by", g_lockedBy ? ModIdOf(g_lockedBy) : g_lockedByBridge ? "bridge" : ""},
                           {"vehicle_hook", g_vhook.fn ? ModIdOf(g_vhook.mod) : "none"}, {"engine", g_engineWhy}, {"steps", g_steps},
                           {"skipped", g_skipped}, {"faults", g_vfaults}});
        } else if (method == "player.lock") {
            if (!LockKnown()) { *reply = "player.lock: not known for this build"; return true; }
            if (!game::Player()) { *reply = "player.lock: no player (no mission)"; return true; }
            if (!PlayerLock(nullptr, p.value("on", true) ? 1 : 0)) { *reply = "player.lock: it faulted"; return true; }
            *reply = dump({{"locked", g_locked}});
        } else {   // vehicle.body
            void *car = nullptr;
            std::string err;
            if (!ParseActor(p.value("car", "car"), &car, &err)) { *reply = "vehicle.body: car: " + err; return true; }
            MmlVehicleBody b{};
            if (!VehicleBody(car, &b, nullptr)) { *reply = "vehicle.body: not a car (or not known for this build)"; return true; }
            auto take = [&](const char *k, float *dst) {
                if (p.contains(k) && p[k].is_array() && p[k].size() == 3) for (int i = 0; i < 3; i++) dst[i] = p[k][i].get<float>();
            };
            bool write = p.contains("pos") || p.contains("velocity") || p.contains("turn");
            take("pos", b.pos);
            take("velocity", b.velocity);
            take("turn", b.turn);
            if (write && !VehicleBody(car, &b, &b)) { *reply = "vehicle.body: not written"; return true; }
            *reply = dump({{"pos", v3(b.pos)}, {"velocity", v3(b.velocity)}, {"turn", v3(b.turn)}, {"forward", v3(b.axes[0])}, {"right", v3(b.axes[1])},
                           {"up", v3(b.axes[2])}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
