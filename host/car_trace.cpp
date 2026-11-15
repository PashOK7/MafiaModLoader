// Vehicle incident recorder. The game thread keeps fixed-size history for the player's car alone.
// A dump is made only on a trigger or explicit request, while the pre-incident history is still intact.
#include "host.h"
#include "mml/game/game.h"
#include "json.hpp"
#include <algorithm>
#include <array>

namespace host {
namespace {
using json = nlohmann::json;
constexpr unsigned kSamples = 512, kMarks = 512;
constexpr ULONGLONG kHistoryMs = 3500, kGainWindowMs = 900, kCooldownMs = 15000;
constexpr float kStepGain = 8.0f, kWindowGain = 12.0f;

struct Wheel { float load, compression; unsigned flags; };
struct Sample {
    unsigned tick;
    ULONGLONG time;
    const void *car;
    int ms, gear, wheelCount;
    unsigned flags;
    float rpm, physicsStep, accelerator, brake;
    V3 pos, up;
    CarTraceMotion before, native, after;
    Wheel wheels[6];
};
enum class Kind { Contact, HumanHit, PedestrianHit, Explosion, Mod };
struct Mark {
    unsigned tick;
    Kind kind;
    const void *actor, *caller;
    unsigned detail;
    MmlVec3 point, vector;
    float value;
    char mod[64];
};
std::array<Sample, kSamples> g_samples;
std::array<Mark, kMarks> g_marks;
Sample g_pending;
unsigned g_written, g_marked, g_window, g_serial, g_active, g_automatic;
ULONGLONG g_lastAutomatic;
void *g_car, *g_player;
static_assert(sizeof(g_samples) + sizeof(g_marks) < 192 * 1024, "vehicle trace memory budget");

template<class V> json Vec(const V &v) { return {v.x, v.y, v.z}; }
V3 V(const MmlVec3 &v) { return {v.x, v.y, v.z}; }
MmlVec3 MV(const V3 &v) { return {v.x, v.y, v.z}; }
std::string Addr(const void *p) {
    char out[16];
    snprintf(out, sizeof out, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return out;
}
std::string TracePath() { return MmlDir() + "logs\\vehicle-trace.json"; }

void Reset() {
    g_written = g_marked = g_window = g_active = 0;
    g_car = g_player = nullptr;
}

CarTraceMotion Motion(unsigned tick) {
    __try {
        game::Car car(g_car);
        return {tick, MV(car.Velocity()), MV(car.AngularVelocity())};
    } __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}

void NativeDetails(Sample &s) {
    __try {
        game::Car car(g_car);
        s.pos = car.Position();
        s.up = car.Up();
        s.gear = car.Gear();
        s.rpm = car.Rpm();
        s.flags = car.Flags();
        s.physicsStep = car.StepTime();
        const int count = std::clamp(car.WheelCount(), 0, static_cast<int>(std::size(s.wheels)));
        for (int i = 0; i < count; ++i) {
            void *wheel = car.Wheel(i);
            if (wheel) s.wheels[i] = {game::at<float>(wheel, game::kWheelLoad),
                                      game::at<float>(wheel, game::kWheelCompression),
                                      game::at<unsigned>(wheel, game::kWheelFlags)};
            ++s.wheelCount;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s.wheelCount = 0; }
}

Mark &Append(Kind kind, const void *actor = nullptr) {
    Mark &m = g_marks[g_marked++ % kMarks];
    m = {};
    m.tick = g_active;
    m.kind = kind;
    m.actor = actor;
    return m;
}

json MotionJson(const CarTraceMotion &m) { return {{"velocity", Vec(m.velocity)}, {"turn", Vec(m.turn)}}; }

bool Save(const char *reason) {
    if (!g_written) return false;
    const Sample &last = g_samples[(g_written - 1) % kSamples];
    unsigned first = g_written > kSamples ? g_written - kSamples : 0;
    while (g_samples[first % kSamples].time + kHistoryMs < last.time) ++first;
    const unsigned firstTick = g_samples[first % kSamples].tick;
    json samples = json::array(), marks = json::array();
    for (unsigned i = first; i < g_written; ++i) {
        const Sample &s = g_samples[i % kSamples];
        json wheels = json::array();
        for (int k = 0; k < s.wheelCount; ++k)
            wheels.push_back({s.wheels[k].load, s.wheels[k].compression, s.wheels[k].flags});
        samples.push_back({{"tick", s.tick}, {"ms_ago", last.time - s.time}, {"mission_step_ms", s.ms},
            {"position_after_native", Vec(s.pos)}, {"up_after_native", Vec(s.up)}, {"rpm", s.rpm}, {"gear", s.gear},
            {"flags", s.flags}, {"vehicle_step_s", s.physicsStep}, {"accelerator", s.accelerator}, {"brake", s.brake},
            {"before_native", MotionJson(s.before)}, {"after_native", MotionJson(s.native)},
            {"after_tick", MotionJson(s.after)}, {"wheels_load_n_compression_m_flags", std::move(wheels)}});
    }
    unsigned firstMark = g_marked > kMarks ? g_marked - kMarks : 0;
    for (unsigned i = firstMark; i < g_marked; ++i) {
        const Mark &m = g_marks[i % kMarks];
        if (m.tick < firstTick || m.tick > last.tick) continue;
        json row = {{"tick", m.tick}};
        if (m.kind == Kind::Mod) {
            row.update({{"kind", "mission_tick_mod"}, {"mod", m.mod},
                        {"delta_velocity", Vec(m.vector)}, {"delta_turn", Vec(m.point)}});
        } else {
            row["actor"] = Addr(m.actor);
            row["position"] = Vec(m.point);
            if (m.kind == Kind::Contact) {
                row.update({{"kind", "contact"}, {"caller", Addr(m.caller)}, {"impulse_ns", Vec(m.vector)},
                            {"impulse_per_mass_m_s", m.value}, {"primitive_flags", m.detail}});
            } else if (m.kind == Kind::Explosion) {
                row.update({{"kind", "explosion"}, {"damage", m.value}});
            } else {
                row.update({{"kind", m.kind == Kind::HumanHit ? "human_hit" : "pedestrian_hit"},
                            {"hit_type", m.detail}, {"damage", m.value}, {"direction", Vec(m.vector)}});
            }
        }
        marks.push_back(std::move(row));
    }
    const std::string path = TracePath(), temp = path + ".tmp";
    json report = {{"schema", 1}, {"build", GameBuild()}, {"mission", MissionName()}, {"pid", GetCurrentProcessId()},
        {"car", Addr(last.car)}, {"trigger", reason}, {"marks_overwritten", firstMark},
        {"samples", std::move(samples)}, {"marks", std::move(marks)}};
    std::string encoded = report.dump(-1, ' ', false, json::error_handler_t::replace);
    FILE *file = FileOpen(temp, "wb");
    if (!file) return false;
    bool wrote = fwrite(encoded.data(), 1, encoded.size(), file) == encoded.size();
    if (fclose(file) != 0) wrote = false;
    if (!wrote || (PathExists(path) && !PathCopy(path, MmlDir() + "logs\\vehicle-trace.prev.json", false)) ||
        !PathMove(temp, path, MOVEFILE_REPLACE_EXISTING)) {
        PathDelete(temp);
        return false;
    }
    HLOG("vehicle trace: %s at tick %u, %u samples: %s", reason, last.tick, g_written - first, path.c_str());
    return true;
}
} // namespace

unsigned CarTraceBegin(int ms) {
    if (!game::g_exe || g_active) return 0;
    void *player = nullptr, *car = nullptr;
    __try { player = game::Player(); car = game::PlayerCar(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!car) return 0;
    if (car != g_car || player != g_player) Reset();
    g_car = car;
    g_player = player;
    g_pending = {};
    g_pending.tick = ++g_serial;
    g_pending.time = GetTickCount64();
    g_pending.car = car;
    g_pending.ms = ms;
    g_pending.before = Motion(g_pending.tick);
    g_active = g_pending.before.tick;
    return g_active;
}

void CarTraceNative(unsigned tick) {
    if (!tick || tick != g_active) return;
    g_pending.native = Motion(tick);
    NativeDetails(g_pending);
    static const int accelerate = ActionSlotOf("Accelerate"), brake = ActionSlotOf("Brake");
    g_pending.accelerator = ActionValue(accelerate, false);
    g_pending.brake = ActionValue(brake, false);
}

void CarTraceEnd(unsigned tick) {
    if (!tick || tick != g_active) return;
    g_active = 0;
    g_pending.after = Motion(tick);
    if (!g_pending.native.tick || !g_pending.after.tick) return;
    const Sample &s = g_pending;
    const float before = Len(V(s.before.velocity));
    const float peak = std::max({before, Len(V(s.native.velocity)), Len(V(s.after.velocity))});
    g_window = std::max(g_window, g_written >= kSamples ? g_written - kSamples + 1 : 0);
    while (g_window < g_written && g_samples[g_window % kSamples].time + kGainWindowMs < s.time) ++g_window;
    const MmlVec3 earlier = g_window < g_written ? g_samples[g_window % kSamples].after.velocity : s.before.velocity;
    const float up = std::max({s.before.velocity.y, s.native.velocity.y, s.after.velocity.y});
    g_samples[g_written++ % kSamples] = s;
    if (g_lastAutomatic && s.time - g_lastAutomatic < kCooldownMs) return;
    const char *reason = peak - before >= kStepGain ? "single_step_speed_gain" :
                         peak - Len(V(earlier)) >= kWindowGain ? "rapid_speed_gain" :
                         up - earlier.y >= kStepGain ? "upward_velocity_gain" : nullptr;
    if (!reason) return;
    g_lastAutomatic = s.time;
    if (Save(reason)) ++g_automatic;
    else HWARN("vehicle trace: could not save %s", reason);
}

CarTraceMotion CarTraceBeforeMod() { return g_active ? Motion(g_active) : CarTraceMotion{}; }
void CarTraceAfterMod(const CarTraceMotion &before, const char *mod) {
    if (before.tick != g_active) return;
    CarTraceMotion after = Motion(g_active);
    if (!after.tick) return;
    V3 dv = V(after.velocity) - V(before.velocity), dw = V(after.turn) - V(before.turn);
    if (Dot(dv, dv) == 0 && Dot(dw, dw) == 0) return;
    Mark &m = Append(Kind::Mod);
    m.vector = MV(dv);
    m.point = MV(dw);
    snprintf(m.mod, sizeof m.mod, "%s", mod);
}

bool CarTraceWantsContact(const void *car) { return g_active && car == g_car; }
void CarTraceContact(const MmlCarCrash &contact, const void *struck, const void *caller) {
    Mark &m = Append(Kind::Contact, struck);
    m.caller = caller;
    m.detail = contact.flags;
    m.point = contact.pos;
    m.vector = contact.impulse;
    m.value = contact.speed;
}

void CarTraceEvent(const MmlEvent &ev) {
    if (ev.type == MML_EVENT_MISSION_CLOSE || ev.type == MML_EVENT_GAME_LOADED) { Reset(); return; }
    if (ev.type == MML_EVENT_ACTOR_DESTROYED && ev.actor && (ev.actor == g_car || ev.actor == g_player)) {
        g_active = 0;
        g_car = g_player = nullptr;
    }
    if (!g_active) return;
    const bool pedestrian = ev.type == MML_EVENT_PEDESTRIAN_HIT;
    if ((pedestrian || ev.type == MML_EVENT_HUMAN_HIT) && ev.hit &&
        (ev.hit->attacker == g_car || ev.actor == g_player)) {
        Mark &m = Append(pedestrian ? Kind::PedestrianHit : Kind::HumanHit, ev.actor);
        m.detail = ev.hit->type;
        m.point = ev.hit->pos;
        m.vector = ev.hit->dir;
        m.value = ev.hit->damage;
    } else if (ev.type == MML_EVENT_EXPLOSION && ev.explosion) {
        const MmlExplosion &e = *ev.explosion;
        __try {
            V3 delta = game::Car(g_car).Position() - V(e.pos);
            if (Dot(delta, delta) > e.radius * e.radius) return;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        Mark &m = Append(Kind::Explosion, e.source);
        m.point = e.pos;
        m.value = e.damage;
    }
}

bool CarTraceMethod(const std::string &params, std::string *reply, bool *ok) {
    const json p = json::parse(params, nullptr, false);
    bool save = p.is_object() && p.contains("save") && p["save"] == true;
    *ok = !save || Save("manual");
    if (!*ok) {
        *reply = g_written ? "vehicle.trace: could not save the capture" : "vehicle.trace: no car history yet";
        return true;
    }
    std::string path = TracePath();
    *reply = json({{"supported", game::g_exe != nullptr}, {"samples", std::min(g_written, kSamples)},
        {"automatic_captures", g_automatic}, {"buffer_bytes", sizeof(g_samples) + sizeof(g_marks)},
        {"file", PathExists(path) ? path : ""}}).dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}
} // namespace host
