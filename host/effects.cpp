// The game's effects as data (mml.h car_exhaust, exhaust_hook, particle_def, particle_def_set; docs/MODDING.md "The
// game's effects").
//
// The car exhaust is C_Vehicle::UpdateExhaustParticles (game_build.h vehExhaust), every tick for each car: when the
// engine runs and the car is slower than its limit, the shared emitter of the car's particle type is moved to each
// pipe and fired (mafia-ragdoll's smoke_game.cpp read it in 1.2; 1.0's is the same code). The loader reads the same
// fields: the pipes from the model's EXHAUST dummies (24-byte {position, direction} records in the car's frame), the
// jet speed between the idle's and the top's by the engine's turns, the car's velocity at each pipe as the game works
// it out (its velocity minus omega x (pipe - centre of mass)), the emitter table's additions. A mod's exhaust_hook is
// told before the game emits and may leave the game's puffs out for that car (its own smoke instead).
//
// tables\particles.def (game_build.h particleDefs): 0xA4-byte records, read and written in place; C_car::LoadParticles
// hands them to the emitters when it makes them, so a change shows from the next mission's load.
//
// Bridge methods (game thread): exhaust.state {car}, particles.defs {index}, particles.def.set {index, ...}.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kDefSize = 0xA4, kEmitterEntry = 0x1C, kPipeRecord = 24;

using ExhaustFn = void(__fastcall *)(void *vehicle, void *edx, int dtMs);
ExhaustFn oExhaust;
bool g_hooked;
std::string g_hookWhy = "not hooked yet";

struct Hook { MmlMod *mod; MmlExhaustFn fn; void *user; } g_hook;
unsigned g_calls, g_skipped, g_faults;

const game::ExeBuild &E() { return game::Exe(); }
bool Known() { return game::g_exe && E().vehExhaust && E().emitterTable && E().particleDefs; }

// The exhaust of the C_Vehicle `v` (its car at v - vehicle), read as the game reads it.
bool ReadExhaust(uint8_t *v, MmlExhaust *e) {
    const game::ExeBuild &x = E();
    std::memset(e, 0, sizeof *e);
    e->size = sizeof *e;
    e->car = v - x.vehicle;
    e->type = game::at<int>(v, x.vehExType);
    e->max_speed = game::at<float>(v, x.vehExMaxSpeed);
    bool running = (game::at<uint32_t>(v, x.vehState) & 0x200) && game::at<uint8_t>(v, x.vehEngineOn);
    e->emitting = running && game::at<float>(v, x.vehSpeed) < e->max_speed && e->type > 0;
    float rpsMax = game::at<float>(v, x.vehRpsMax);
    float t = rpsMax > 0 ? game::at<float>(v, x.vehRps) / rpsMax : 0;
    float jmin = game::at<float>(v, x.vehExJet), jmax = game::at<float>(v, x.vehExJet + 4);
    e->jet_speed = t * (jmax - jmin) + jmin;
    const uint8_t *b = game::at<uint8_t *>(v, x.vehExPipes), *end = game::at<uint8_t *>(v, x.vehExPipes + 4);
    const float *m = reinterpret_cast<const float *>(v + x.vehMatrix);
    V3 vel = game::at<V3>(v, x.vehVelocity), w = game::at<V3>(v, x.vehTurn), com = game::at<V3>(v, x.vehCentre);
    if (b && end > b && (end - b) % kPipeRecord == 0 && end - b <= kPipeRecord * 32) {
        for (const uint8_t *r = b; r < end && e->pipes < MML_EXHAUST_PIPES; r += kPipeRecord) {
            const float *f = reinterpret_cast<const float *>(r);
            V3 p{f[0] * m[0] + f[1] * m[4] + f[2] * m[8] + m[12], f[0] * m[1] + f[1] * m[5] + f[2] * m[9] + m[13],
                 f[0] * m[2] + f[1] * m[6] + f[2] * m[10] + m[14]};
            V3 d{f[3] * m[0] + f[4] * m[4] + f[5] * m[8], f[3] * m[1] + f[4] * m[5] + f[5] * m[9], f[3] * m[2] + f[4] * m[6] + f[5] * m[10]};
            float l = std::sqrt(Dot(d, d));
            d = l > 1e-6f ? d * (-1.0f / l) : V3{0, 0, -1};   // (the game blows against the dummy's direction)
            V3 c = vel - Cross(w, p - com);
            int i = e->pipes++;
            e->pos[i][0] = p.x; e->pos[i][1] = p.y; e->pos[i][2] = p.z;
            e->dir[i][0] = d.x; e->dir[i][1] = d.y; e->dir[i][2] = d.z;
            e->carry[i][0] = c.x; e->carry[i][1] = c.y; e->carry[i][2] = c.z;
        }
    }
    const uint8_t *eb = *reinterpret_cast<uint8_t *const *>(x.emitterTable), *ee = *reinterpret_cast<uint8_t *const *>(x.emitterTable + 4);
    if (eb && ee > eb && e->type > 0 && static_cast<size_t>(e->type) < static_cast<size_t>(ee - eb) / kEmitterEntry) {
        const float *en = reinterpret_cast<const float *>(eb + static_cast<size_t>(e->type) * kEmitterEntry);
        for (int i = 0; i < 3; i++) { e->add_birth[i] = en[1 + i]; e->add_death[i] = en[4 + i]; }
    }
    e->death_share = *reinterpret_cast<const float *>(x.exEndShare);
    return true;
}
bool SafeRead(uint8_t *v, MmlExhaust *e) {
    __try { return ReadExhaust(v, e); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct HookCall { const MmlExhaust *e; int r; };
void CallHook(void *p) {
    auto *c = static_cast<HookCall *>(p);
    c->r = g_hook.fn(c->e, g_hook.user);
}

void __fastcall hkExhaust(void *veh, void *edx, int dtMs) {
    g_calls++;
    if (g_hook.fn && ModAlive(g_hook.mod)) {
        MmlExhaust e;
        if (SafeRead(static_cast<uint8_t *>(veh), &e)) {
            e.dt_ms = dtMs;
            HookCall c{&e, 0};
            if (!ModCall(g_hook.mod, CallHook, &c, "its exhaust hook")) g_faults++;
            else if (c.r) { g_skipped++; return; }
        }
    }
    oExhaust(veh, edx, dtMs);
}

bool EnsureHook(std::string *err) {
    if (g_hooked) return true;
    if (!Known()) { *err = g_hookWhy = "this build's exhaust is not known"; return false; }
    if (!HookAdd(nullptr, E().vehExhaust, reinterpret_cast<void *>(&hkExhaust), reinterpret_cast<void **>(&oExhaust), err)) {
        g_hookWhy = *err;
        return false;
    }
    g_hooked = true;
    g_hookWhy = "hooked";
    return true;
}

uint8_t *DefRecord(int index, int *count) {
    uint8_t *recs = *reinterpret_cast<uint8_t **>(E().particleDefs);
    int n = *reinterpret_cast<int *>(E().particleDefs + 4);
    if (count) *count = recs && n > 0 && n < 4096 ? n : 0;
    if (!recs || index < 0 || index >= n || n >= 4096) return nullptr;
    return recs + static_cast<size_t>(index) * kDefSize;
}

void DefFrom(const uint8_t *r, MmlParticleDef *d) {
    std::memset(d, 0, sizeof *d);
    d->size = sizeof *d;
    memcpy(d->raw, r, kDefSize);
    memcpy(d->model, r, 31);
    auto i32 = [&](int o) { return *reinterpret_cast<const int *>(r + o); };
    auto f32 = [&](int o) { return *reinterpret_cast<const float *>(r + o); };
    d->interval_ms = i32(0x20); d->interval_rand_ms = i32(0x24);
    d->life_ms = i32(0x28); d->life_rand_ms = i32(0x2C);
    d->growth_min = f32(0x30); d->growth_max = f32(0x34);
    d->birth_cube = f32(0x38); d->dir_jitter = f32(0x3C);
    d->turn_ms = i32(0x40); d->turn_rand_ms = i32(0x44);
    d->fade_in = f32(0x4C); d->fade_out = f32(0x50);
    d->spin_min = f32(0x7C); d->spin_max = f32(0x80);
    d->scale = f32(0x84);
    for (int i = 0; i < 3; i++) { d->tint_birth[i] = f32(0x8C + 4 * i); d->tint_death[i] = f32(0x98 + 4 * i); }
}
void DefTo(const MmlParticleDef *d, uint8_t *r) {
    auto i32 = [&](int o, int v) { *reinterpret_cast<int *>(r + o) = v; };
    auto f32 = [&](int o, float v) { *reinterpret_cast<float *>(r + o) = v; };
    i32(0x20, d->interval_ms); i32(0x24, d->interval_rand_ms);
    i32(0x28, d->life_ms); i32(0x2C, d->life_rand_ms);
    f32(0x30, d->growth_min); f32(0x34, d->growth_max);
    f32(0x38, d->birth_cube); f32(0x3C, d->dir_jitter);
    i32(0x40, d->turn_ms); i32(0x44, d->turn_rand_ms);
    f32(0x4C, d->fade_in); f32(0x50, d->fade_out);
    f32(0x7C, d->spin_min); f32(0x80, d->spin_max);
    f32(0x84, d->scale);
    for (int i = 0; i < 3; i++) { f32(0x8C + 4 * i, d->tint_birth[i]); f32(0x98 + 4 * i, d->tint_death[i]); }
}
bool SafeDef(int index, MmlParticleDef *out, int *count) {
    __try {
        uint8_t *r = DefRecord(index, count);
        if (!r || !out) return r != nullptr;
        DefFrom(r, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeDefSet(int index, const MmlParticleDef *d) {
    __try {
        uint8_t *r = DefRecord(index, nullptr);
        if (!r) return false;
        DefTo(d, r);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the game's particles: the two slots of LS3D's particle visuals' vtables (game_build.h prtbVtable, prteVtable)
// that every particle goes through: Update (+0x18) after the game has moved it, SetParent (+0x2C) when its life
// starts or ends. Patched in the vtables themselves, so only particles pay (the frame's own functions are shared by
// every frame of the scene); the original is the frame's, the same in both vtables.
constexpr int kVtUpdateSlot = 0x18, kVtParentSlot = 0x2C, kFramePosLocal = 0x80;
constexpr uint32_t kFlagPinned = 0x80;
using UpdateFn = void(__stdcall *)(void *frame);
using SetParentFn = int(__stdcall *)(void *frame, void *parent, uint32_t flags);
UpdateFn oPrtUpdate;
SetParentFn oPrtParent;
bool g_prtPatched;
std::string g_prtWhy = "not patched yet";
struct PrtHook { MmlMod *mod; MmlParticleFn fn; void *user; } g_prtHook;
struct Track { V3 world; DWORD t; };
std::unordered_map<void *, Track> g_tracks;
unsigned long long g_prtMoves[3], g_prtMoved, g_prtLives, g_prtFaults;
DWORD g_prtPruned;
uintptr_t g_prtbVt, g_prteVt;

const float *ParentWorld(void *frame) {
    void *parent = game::at<void *>(frame, game::kFrameParent);
    return parent ? game::WorldMatrix(parent) : nullptr;
}
V3 ToWorld(const float *m, const V3 &p) {
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13], p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}
bool ToLocal(const float *m, const V3 &w, V3 *out) {   // solves t + r0 x + r1 y + r2 z = w (Cramer)
    V3 r0{m[0], m[1], m[2]}, r1{m[4], m[5], m[6]}, r2{m[8], m[9], m[10]}, c = w - V3{m[12], m[13], m[14]};
    float det = Dot(r0, Cross(r1, r2));
    if (std::fabs(det) < 1e-10f) return false;
    *out = {Dot(c, Cross(r1, r2)) / det, Dot(r0, Cross(c, r2)) / det, Dot(r0, Cross(r1, c)) / det};
    return true;
}

struct PrtCall { MmlParticle *p; int r; };
void CallPrt(void *x) {
    auto *c = static_cast<PrtCall *>(x);
    c->r = g_prtHook.fn(c->p, g_prtHook.user);
}

void ParticleMoved(uint8_t *f) {
    if (game::at<uint32_t>(f, game::kFrameFlags) & kFlagPinned) return;
    const float *m = ParentWorld(f);
    if (!m) return;
    V3 &local = game::at<V3>(f, kFramePosLocal);
    V3 w = ToWorld(m, local);
    DWORD now = GetTickCount();
    MmlParticle p{};
    p.size = sizeof p;
    p.frame = f;
    p.kind = game::at<uintptr_t>(f, 0) == g_prteVt ? MML_PARTICLE_EMITTED : MML_PARTICLE_POOLED;
    g_prtMoves[p.kind]++;
    auto it = g_tracks.find(f);
    p.first = it == g_tracks.end();
    if (!p.first) {
        p.dt_ms = static_cast<int>(now - it->second.t);
        V3 d = w - it->second.world;
        p.moved[0] = d.x; p.moved[1] = d.y; p.moved[2] = d.z;
    }
    p.pos[0] = w.x; p.pos[1] = w.y; p.pos[2] = w.z;
    if (g_prtHook.fn && ModAlive(g_prtHook.mod)) {
        PrtCall c{&p, 0};
        if (!ModCall(g_prtHook.mod, CallPrt, &c, "its particle hook")) g_prtFaults++;
        else if (c.r) {
            V3 want{p.pos[0], p.pos[1], p.pos[2]}, loc;
            if (std::isfinite(want.x + want.y + want.z) && ToLocal(m, want, &loc)) {
                local = loc;
                uint32_t &fl = game::at<uint32_t>(f, game::kFrameFlags);
                fl = (fl & 0xFFFFFEDFu) | 0x40000000u;   // (as the game's SetPos marks a move)
                w = want;
                g_prtMoved++;
            }
        }
    }
    g_tracks[f] = Track{w, now};
    if (now - g_prtPruned > 2000) {   // (lives whose end was not seen: unlinked by a direct call)
        g_prtPruned = now;
        for (auto i = g_tracks.begin(); i != g_tracks.end();) i = now - i->second.t > 5000 ? g_tracks.erase(i) : std::next(i);
    }
}
void SafeParticleMoved(uint8_t *f) { __try { ParticleMoved(f); } __except (EXCEPTION_EXECUTE_HANDLER) { g_prtFaults++; } }

void __stdcall hkPrtUpdate(void *frame) {
    if (g_prtHook.fn) SafeParticleMoved(static_cast<uint8_t *>(frame));
    else g_prtMoves[game::at<uintptr_t>(frame, 0) == g_prteVt ? MML_PARTICLE_EMITTED : MML_PARTICLE_POOLED]++;
    oPrtUpdate(frame);
}
int __stdcall hkPrtParent(void *frame, void *parent, uint32_t flags) {
    // into the scene or out of it: a life starts or ends (moving between sectors is neither)
    if (!parent || !game::at<void *>(frame, game::kFrameParent)) {
        g_tracks.erase(frame);
        g_prtLives++;
    }
    return oPrtParent(frame, parent, flags);
}

bool PatchSlot(uintptr_t slot, void *hook, void **orig) {
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void *>(slot), 4, PAGE_READWRITE, &old)) return false;
    *orig = *reinterpret_cast<void **>(slot);
    *reinterpret_cast<void **>(slot) = hook;
    VirtualProtect(reinterpret_cast<void *>(slot), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(slot), 4);
    return true;
}
bool EnsureParticles(std::string *err) {
    if (g_prtPatched) return true;
    if (!game::g_ls3d || !game::Ls3d().prtbVtable || !game::Ls3d().prteVtable) { *err = g_prtWhy = "this build's particles are not known"; return false; }
    uintptr_t base = ModuleBase("LS3DF.dll");
    g_prtbVt = base + game::Ls3d().prtbVtable;
    g_prteVt = base + game::Ls3d().prteVtable;
    void *u1 = *reinterpret_cast<void **>(g_prtbVt + kVtUpdateSlot), *u2 = *reinterpret_cast<void **>(g_prteVt + kVtUpdateSlot);
    void *s1 = *reinterpret_cast<void **>(g_prtbVt + kVtParentSlot), *s2 = *reinterpret_cast<void **>(g_prteVt + kVtParentSlot);
    if (u1 != u2 || s1 != s2) { *err = g_prtWhy = "the two particle vtables' slots differ (patched by someone else?)"; return false; }
    oPrtUpdate = reinterpret_cast<UpdateFn>(u1);   // (before the slots point at the hooks)
    oPrtParent = reinterpret_cast<SetParentFn>(s1);
    void *o = nullptr;
    bool ok = PatchSlot(g_prtbVt + kVtUpdateSlot, reinterpret_cast<void *>(&hkPrtUpdate), &o) &&
              PatchSlot(g_prteVt + kVtUpdateSlot, reinterpret_cast<void *>(&hkPrtUpdate), &o);
    ok = ok && PatchSlot(g_prtbVt + kVtParentSlot, reinterpret_cast<void *>(&hkPrtParent), &o) &&
         PatchSlot(g_prteVt + kVtParentSlot, reinterpret_cast<void *>(&hkPrtParent), &o);
    if (!ok) { *err = g_prtWhy = "the vtables could not be written"; return false; }
    g_prtPatched = true;
    g_prtWhy = "patched";
    return true;
}

// ---- the weather: LS3D's weather system of the mission's scene (C_mission +0x10), its getter and setter through the
// scene's vtable, a dword a parameter: a flag, an int or a float's bits (as each case of the setter reads it).
constexpr int kVtWeatherSet = 0xE8, kVtWeatherGet = 0xEC, kVtWeatherReset = 0xF0;
enum WType { kFlag, kInt, kColor, kFloat };
const WType kWeatherTypes[MML_WEATHER_PARAMS] = {kFlag, kFlag, kFlag, kColor, kColor, kFloat, kFloat, kFloat, kFloat, kFloat,
                                                 kInt, kFloat, kFloat, kFloat, kFlag};
const char *const kWeatherNames[MML_WEATHER_PARAMS] = {"on", "sectors", "dummies", "color_high", "color_low", "speed", "length", "width",
                                                       "max_dist", "max_height", "max_count", "dir_x", "dir_y", "dir_z", "mode"};
using WeatherSetFn = void(__stdcall *)(void *scene, int param, uint32_t value);
using WeatherGetFn = uint32_t(__stdcall *)(void *scene, int param);
using WeatherResetFn = void(__stdcall *)(void *scene);
using WeatherSaveFn = void(__thiscall *)(void *game, bool save, int param);

void *Scene() {
    void *m = game::Mission();
    return m ? game::at<void *>(m, game::kMissionScene) : nullptr;
}
bool SceneIsLs3d(void *scene) {
    return scene && game::g_ls3d && game::Ls3d().sceneVtable && game::at<uintptr_t>(scene, 0) == ModuleBase("LS3DF.dll") + game::Ls3d().sceneVtable;
}
bool SafeWeatherGet(int param, double *out) {
    __try {
        void *s = Scene();
        if (!SceneIsLs3d(s)) return false;
        uint32_t v = reinterpret_cast<WeatherGetFn>((*reinterpret_cast<void ***>(s))[kVtWeatherGet / 4])(s, param);
        float f;
        memcpy(&f, &v, 4);
        switch (kWeatherTypes[param]) {
        case kFlag: *out = v ? 1 : 0; break;
        case kInt: *out = static_cast<int32_t>(v); break;
        case kColor: *out = v; break;
        case kFloat: *out = f; break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWeatherSet(int param, double value) {
    __try {
        void *s = Scene();
        if (!SceneIsLs3d(s)) return false;
        uint32_t v = 0;
        switch (kWeatherTypes[param]) {
        case kFlag: v = value != 0 ? 1 : 0; break;
        case kInt: v = static_cast<uint32_t>(static_cast<int32_t>(value)); break;
        case kColor: v = static_cast<uint32_t>(static_cast<int64_t>(value)); break;
        case kFloat: { float f = static_cast<float>(value); memcpy(&v, &f, 4); break; }
        }
        reinterpret_cast<WeatherSetFn>((*reinterpret_cast<void ***>(s))[kVtWeatherSet / 4])(s, param, v);
        // (as WEATHER_SETPARAM: On and MaxCount kept and applied again scaled by the detail setting)
        if ((param == MML_WEATHER_ON || param == MML_WEATHER_MAX_COUNT) && E().weatherSave && game::Game())
            reinterpret_cast<WeatherSaveFn>(E().weatherSave)(game::Game(), true, param);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWeatherReset() {
    __try {
        void *s = Scene();
        if (!SceneIsLs3d(s)) return false;
        reinterpret_cast<WeatherResetFn>((*reinterpret_cast<void ***>(s))[kVtWeatherReset / 4])(s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string HexOf(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}
json R3(const float *v) { return json::array({std::round(v[0] * 1000) / 1000, std::round(v[1] * 1000) / 1000, std::round(v[2] * 1000) / 1000}); }
json DefJson(int index, const MmlParticleDef &d) {
    return {{"index", index}, {"model", d.model}, {"interval_ms", {d.interval_ms, d.interval_rand_ms}}, {"life_ms", {d.life_ms, d.life_rand_ms}},
            {"growth", {d.growth_min, d.growth_max}}, {"birth_cube", d.birth_cube}, {"dir_jitter", d.dir_jitter},
            {"turn_ms", {d.turn_ms, d.turn_rand_ms}}, {"fade", {d.fade_in, d.fade_out}}, {"spin", {d.spin_min, d.spin_max}},
            {"scale", d.scale}, {"tint_birth", R3(d.tint_birth)}, {"tint_death", R3(d.tint_death)}};
}

} // namespace

namespace {
bool IsCar(void *a) {
    __try { return game::at<int>(a, game::kActorType) == game::kActorTypeCar; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

int CarExhaust(void *car, MmlExhaust *out) {
    if (!Known() || !car || !out || !IsCar(car)) return 0;
    return SafeRead(static_cast<uint8_t *>(car) + E().vehicle, out) ? 1 : 0;
}

int ExhaustHook(MmlMod *mod, MmlExhaustFn fn, void *user) {
    if (!fn) { if (g_hook.mod == mod) g_hook = Hook{}; return 1; }
    std::string err;
    if (!EnsureHook(&err)) { Log(MML_LOG_WARN, ModIdOf(mod), "exhaust_hook: %s", err.c_str()); return 0; }
    g_hook = Hook{mod, fn, user};
    return 1;
}
void EffectsModGone(MmlMod *mod) {
    if (g_hook.mod == mod) g_hook = Hook{};
    if (g_prtHook.mod == mod) g_prtHook = PrtHook{};
}
void EffectsMissionClosed() { g_tracks.clear(); }

int WeatherGet(int param, double *value) {
    if (!game::g_exe || param < 0 || param >= MML_WEATHER_PARAMS || !value) return 0;
    return SafeWeatherGet(param, value) ? 1 : 0;
}
int WeatherSet(int param, double value) {
    if (!game::g_exe || param < 0 || param >= MML_WEATHER_PARAMS || !std::isfinite(value)) return 0;
    return SafeWeatherSet(param, value) ? 1 : 0;
}
int WeatherReset() { return game::g_exe && SafeWeatherReset() ? 1 : 0; }

int ParticleHook(MmlMod *mod, MmlParticleFn fn, void *user) {
    if (!fn) { if (g_prtHook.mod == mod) g_prtHook = PrtHook{}; return 1; }
    std::string err;
    if (!EnsureParticles(&err)) { Log(MML_LOG_WARN, ModIdOf(mod), "particle_hook: %s", err.c_str()); return 0; }
    g_tracks.clear();
    g_prtHook = PrtHook{mod, fn, user};
    return 1;
}

int ParticleDef(int index, MmlParticleDef *out) {
    if (!Known()) return 0;
    int count = 0;
    if (index < 0) { SafeDef(-1, nullptr, &count); return count; }
    return SafeDef(index, out, &count) ? 1 : 0;
}
int ParticleDefSet(int index, const MmlParticleDef *d) { return Known() && d && SafeDefSet(index, d) ? 1 : 0; }

bool EffectsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "exhaust.state" && method != "particles.defs" && method != "particles.def.set" && method != "particles.state" &&
        method != "weather.state" && method != "weather.set" && method != "weather.reset") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (!Known() && method.compare(0, 8, "weather.") != 0) { *reply = method + ": this build's effects are not known"; return true; }
        if (method == "weather.state" || method == "weather.set" || method == "weather.reset") {
            if (method == "weather.reset" && !WeatherReset()) { *reply = "weather.reset: no mission's scene"; return true; }
            if (method == "weather.set") {
                int set = 0;
                for (int i = 0; i < MML_WEATHER_PARAMS; i++) {
                    if (!p.contains(kWeatherNames[i])) continue;
                    const json &v = p[kWeatherNames[i]];
                    double d = v.is_boolean() ? (v.get<bool>() ? 1.0 : 0.0) : v.is_string() ? static_cast<double>(ParseAddress(v.get<std::string>())) : v.get<double>();
                    if (!WeatherSet(i, d)) { *reply = std::string("weather.set: ") + kWeatherNames[i] + ": no mission's scene"; return true; }
                    set++;
                }
                if (!set) { *reply = "weather.set: any of on, sectors, dummies, color_high, color_low, speed, length, width, max_dist, max_height, max_count, dir_x, dir_y, dir_z, mode"; return true; }
            }
            json out = json::object();
            for (int i = 0; i < MML_WEATHER_PARAMS; i++) {
                double v;
                if (!WeatherGet(i, &v)) { *reply = method + ": no mission's scene"; return true; }
                if (kWeatherTypes[i] == kColor) { char b[16]; snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(static_cast<int64_t>(v))); out[kWeatherNames[i]] = b; }
                else if (kWeatherTypes[i] == kFloat) out[kWeatherNames[i]] = std::round(v * 10000) / 10000;
                else out[kWeatherNames[i]] = static_cast<int64_t>(v);
            }
            *reply = dump(out);
            *ok = true;
            return true;
        }
        if (method == "particles.state") {
            if (p.value("patch", false) && !EnsureParticles(&err)) { *reply = "particles.state: " + err; return true; }
            *reply = dump({{"patched", g_prtPatched}, {"why", g_prtWhy}, {"hook", g_prtHook.fn ? ModIdOf(g_prtHook.mod) : "none"},
                           {"moves_pooled", g_prtMoves[MML_PARTICLE_POOLED]}, {"moves_emitted", g_prtMoves[MML_PARTICLE_EMITTED]},
                           {"moved_by_hook", g_prtMoved}, {"lives_started_or_ended", g_prtLives}, {"tracked", g_tracks.size()},
                           {"faults", g_prtFaults}, {"sample", [&] {
                                json s = json::array();   // (a few tracked particles: where they were last moved to)
                                for (const auto &kv : g_tracks) {
                                    if (s.size() >= 5) break;
                                    float w[3] = {kv.second.world.x, kv.second.world.y, kv.second.world.z};
                                    s.push_back({{"frame", HexOf(kv.first)}, {"pos", R3(w)}, {"ms_ago", GetTickCount() - kv.second.t}});
                                }
                                return s;
                            }()}});
            *ok = true;
            return true;
        }
        if (method == "exhaust.state") {
            json out = {{"hooked", g_hooked}, {"hook", g_hook.fn ? ModIdOf(g_hook.mod) : "none"}, {"calls", g_calls}, {"skipped", g_skipped},
                        {"faults", g_faults}, {"why", g_hookWhy}};
            std::string a = p.value("car", "car");
            void *car = nullptr;
            if (ParseActor(a, &car, &err)) {
                MmlExhaust e;
                if (CarExhaust(car, &e)) {
                    json pipes = json::array();
                    for (int i = 0; i < e.pipes; i++) pipes.push_back({{"pos", R3(e.pos[i])}, {"dir", R3(e.dir[i])}, {"carry", R3(e.carry[i])}});
                    out["car"] = {{"car", HexOf(e.car)}, {"type", e.type}, {"emitting", e.emitting != 0}, {"jet_speed", e.jet_speed},
                                  {"max_speed", e.max_speed}, {"pipes", pipes}, {"add_birth", R3(e.add_birth)}, {"add_death", R3(e.add_death)},
                                  {"death_share", e.death_share}};
                } else {
                    out["car_note"] = a + " is not a car";
                }
            } else {
                out["car_note"] = err;
            }
            *reply = dump(out);
        } else if (method == "particles.defs") {
            int n = ParticleDef(-1, nullptr);
            json list = json::array();
            MmlParticleDef d;
            if (p.contains("index")) {
                int i = p["index"].get<int>();
                if (!ParticleDef(i, &d)) { *reply = "particles.defs: no record " + std::to_string(i) + " (0.." + std::to_string(n - 1) + ")"; return true; }
                list.push_back(DefJson(i, d));
            } else {
                for (int i = 0; i < n; i++)
                    if (ParticleDef(i, &d)) list.push_back({{"index", i}, {"model", d.model}, {"life_ms", d.life_ms}, {"interval_ms", d.interval_ms}});
            }
            *reply = dump({{"count", n}, {"records", list}});
        } else {
            int i = p.value("index", -1);
            MmlParticleDef d;
            if (!ParticleDef(i, &d)) { *reply = "particles.def.set: no record " + std::to_string(i); return true; }
            auto pair = [&](const char *k, auto *a, auto *b) {
                if (p.contains(k) && p[k].is_array() && p[k].size() == 2) { *a = p[k][0].get<std::remove_pointer_t<decltype(a)>>(); *b = p[k][1].get<std::remove_pointer_t<decltype(b)>>(); }
            };
            auto three = [&](const char *k, float *v) {
                if (p.contains(k) && p[k].is_array() && p[k].size() == 3) for (int j = 0; j < 3; j++) v[j] = p[k][j].get<float>();
            };
            pair("interval_ms", &d.interval_ms, &d.interval_rand_ms);
            pair("life_ms", &d.life_ms, &d.life_rand_ms);
            pair("growth", &d.growth_min, &d.growth_max);
            pair("turn_ms", &d.turn_ms, &d.turn_rand_ms);
            pair("fade", &d.fade_in, &d.fade_out);
            pair("spin", &d.spin_min, &d.spin_max);
            if (p.contains("birth_cube")) d.birth_cube = p["birth_cube"].get<float>();
            if (p.contains("dir_jitter")) d.dir_jitter = p["dir_jitter"].get<float>();
            if (p.contains("scale")) d.scale = p["scale"].get<float>();
            three("tint_birth", d.tint_birth);
            three("tint_death", d.tint_death);
            if (!ParticleDefSet(i, &d)) { *reply = "particles.def.set: the write faulted"; return true; }
            ParticleDef(i, &d);
            json out = DefJson(i, d);
            out["note"] = "the game's emitters take it when they are made: from the next mission's load";
            *reply = dump(out);
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
