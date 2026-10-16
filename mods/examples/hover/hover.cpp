// hover: a car's physics replaced by a mod's (vehicle_hook, action_capture; docs/MODDING.md "Control").
//
// F8 and the player's car floats: the game's step for it is skipped (vehicle_hook returns 1) and the mod moves its
// body itself: it holds `height` metres above the ground under it (the game's own ray, world_ray), Accelerate and
// Brake push it along its nose up to `speed`, the steering turns it about the world's up. Accelerate, Brake and the
// steering are kept from the game while it hovers (action_capture: the game's own driving gets 0) and read by the
// mod (action_held). F8 again: the game's physics takes the car back where it is (it falls to the ground).
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool g_on;
float g_height = 1.5f, g_top = 25, g_turnRate = 1.2f;
float g_speed;           // m/s along the nose now
float g_y;               // the height it floats at now (eased toward the ground's + height)
int g_accel = -1, g_brake = -1, g_left = -1, g_right = -1;
unsigned g_steps;

float Held(int slot) {   // the action, its primary or secondary binding (the next slot)
    if (slot < 0) return 0;
    float a = H->action_held(slot), b = H->action_held(slot + 1);
    return a > b ? a : b;
}

void Capture(bool on) {
    for (int s : {g_accel, g_brake, g_left, g_right})
        if (s >= 0) H->action_capture(M, s, on ? 1 : 0);
}

int OnStep(MmlVehicleStep *st, void *) {
    if (!g_on || !st->player) return 0;   // (every other car: the game's own)
    g_steps++;
    MmlVehicleBody &b = st->body;
    float dt = st->dt > 0 && st->dt < 0.2f ? st->dt : 0.016f;
    // speed along the nose, eased toward what the pedals ask
    float want = (Held(g_accel) - Held(g_brake)) * g_top;
    g_speed += (want - g_speed) * (1.0f - std::exp(-dt * 1.5f));
    // the turn about the world's up: the nose and the right axis turned, up kept up
    float yaw = (Held(g_right) - Held(g_left)) * g_turnRate * (g_speed < 0 ? -1.0f : 1.0f) * std::fmin(1.0f, std::fabs(g_speed) / 3.0f);
    float c = std::cos(yaw * dt), s = std::sin(yaw * dt);
    float fx = b.axes[0][0], fz = b.axes[0][2];
    float n = std::sqrt(fx * fx + fz * fz);
    if (n < 1e-4f) { fx = 0; fz = 1; n = 1; }
    fx /= n; fz /= n;
    float nfx = fx * c + fz * s, nfz = -fx * s + fz * c;   // (turned to the right for a positive yaw: y up)
    b.axes[0][0] = nfx; b.axes[0][1] = 0; b.axes[0][2] = nfz;
    b.axes[2][0] = 0; b.axes[2][1] = 1; b.axes[2][2] = 0;
    b.axes[1][0] = nfz; b.axes[1][1] = 0; b.axes[1][2] = -nfx;   // right = up x forward
    // the ground under it, and the height eased toward it
    const float from[3] = {b.pos[0], b.pos[1] + 2, b.pos[2]}, to[3] = {b.pos[0], b.pos[1] - 30, b.pos[2]};
    MmlHit hit{};
    hit.size = sizeof hit;
    float target = H->world_ray(M, from, to, MML_WORLD_STATIC, nullptr, &hit) ? hit.pos[1] + g_height : b.pos[1];
    g_y += (target - g_y) * (1.0f - std::exp(-dt * 4.0f));
    float vy = (g_y - b.pos[1]) / dt;
    b.pos[0] += nfx * g_speed * dt;
    b.pos[2] += nfz * g_speed * dt;
    b.pos[1] = g_y;
    b.velocity[0] = nfx * g_speed; b.velocity[1] = vy; b.velocity[2] = nfz * g_speed;
    b.turn[0] = 0; b.turn[1] = -yaw; b.turn[2] = 0;
    return 1;
}

void Toggle(void *) {
    g_on = !g_on;
    if (g_on) {
        g_speed = 0;
        g_y = NAN;   // (the height it starts from: the car's body at the hook's first step)
    }
    Capture(g_on);
    H->log(M, MML_LOG_INFO, "hover %s", g_on ? "on" : "off");
}

int OnStepFirst(MmlVehicleStep *st, void *u) {
    if (g_on && st->player && std::isnan(g_y)) g_y = st->body.pos[1];
    return OnStep(st, u);
}

void ReadSettings() {
    g_height = static_cast<float>(H->config_number(M, "height", 1.5));
    g_top = static_cast<float>(H->config_number(M, "speed", 25));
    g_turnRate = static_cast<float>(H->config_number(M, "turn", 1.2));
    H->hotkey(M, H->config_string(M, "hotkey", "F8"), Toggle, nullptr);
}
void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_CLOSE && g_on) { g_on = false; Capture(false); }
}

int Status(const char *, MmlReply *r, void *) {
    char out[160];
    snprintf(out, sizeof out, "{\"on\":%s,\"speed\":%.2f,\"height\":%.2f,\"steps\":%u}", g_on ? "true" : "false", g_speed, g_y, g_steps);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int Cmd(const char *a, MmlReply *r, void *u) {
    Toggle(nullptr);
    return Status(a, r, u);
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, vehicle_body)) return 1;   // a loader without control
    g_accel = H->action_slot("Accelerate");
    g_brake = H->action_slot("Brake");
    g_left = H->action_slot("Steer Left");
    g_right = H->action_slot("Steer Right");
    ReadSettings();
    if (!H->vehicle_hook(M, OnStepFirst, nullptr)) return 1;   // (the log says why)
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->command(mod, "status", "on, its speed, the height it floats at, the steps it made", Status, nullptr);
    host->command(mod, "toggle", "hovering on or off (the hotkey's)", Cmd, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}   // (the host drops its hook and lets its captures go)
