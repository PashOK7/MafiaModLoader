// Native layouts of Mafia's Game.exe and LS3DF.dll, and the calls into them. The addresses differ between the game's
// builds (1.0, 1.2): they come from game_build.h (game::Exe(), game::Ls3d()), set by game::Detect. The layouts here
// are the same in both builds. Every address and layout was checked in our own disassembly (see MODLOG.md,
// docs/VERSIONS.md).
#pragma once
#include <cstdint>
#include <vector>
#include "game_build.h"
#include "mathx.h"

namespace game {

constexpr uint32_t kTestLineSolid    = 0x600000;  // flags the bullet code uses (exclusion mask, 0x20 is always added)

struct RayStats { unsigned rays, hits; V3 lastFrom, lastTo, lastHit, lastNormal; uint32_t lastPrimFlags; };
inline RayStats g_rayStats{};
inline uint32_t g_rayFlags = kTestLineSolid;

// I3D_frame
constexpr int kFrameWorld  = 0x10;   // float[16], row-major, translation in [12..14]
constexpr int kFrameLocal  = 0x50;   // float[16]
constexpr int kFrameScale  = 0x90;   // float[3]
constexpr int kFrameRot    = 0x9C;   // quat w,x,y,z
constexpr int kFrameFlags  = 0xAC;
constexpr int kFrameName   = 0x100;
constexpr int kFrameParent = 0x120;
constexpr int kFrameType   = 0x110;
constexpr int kModelFile   = 0x154;  // a model frame's (type 9) file, as its Open was given it ("Models\\Tommy.i3d"), or null
constexpr uint32_t kFlagScaled = 0x4, kFlagRotValid = 0x8, kFlagLocalBuilt = 0x10, kFlagWorldValid = 0x20,
                   kFlagBoundValid = 0x100, kFlagConstructed = 0x40000000;
constexpr int kVtFindChild = 0x38;   // stdcall(I3D_frame* this, const char* name, uint flags) -> I3D_frame*

// C_actor / C_human
constexpr int kActorType  = 0x10;
constexpr int kActorPos   = 0x24;
constexpr int kActorFacing = 0x30;   // V3, horizontal facing direction
constexpr int kActorAlive = 0x5D;
constexpr int kActorDead  = 0x5E;
constexpr int kActorFrame = 0x68;
constexpr int kHumanHealth = 0x644;
constexpr int kHumanHealthMax = 0x604;   // (the properties' maxima: +0x600..; the current ones +0x640..)
// G_Inventory, embedded at C_human +0x480 (both builds): +0x08 the extra items' count, +0x20 the item in the hand,
// +0x30 five belt slots, +0x80 the coat's (a big weapon), +0x90 four extra items, +0xD0 the one put away in a car;
// each an S_GameItem of 0x10 bytes: u16 id (0: empty), u16, u32 rounds in it, u32 in reserve, u32.
constexpr int kHumanInventory = 0x480;
// The fall (C_human::MovementIntern, both builds): the state +0x40C (game_build.h humanState) is 3 while he is in the
// air; each frame the fall's speed +0x418 (m/s, down) grows by 9.81 t; on landing the state turns 1 and a speed of 8
// or more hurts him (a hit of type 6, (speed - 2) * 10).
constexpr int kHumanFallSpeed = 0x418, kHumanStateAir = 3;

// C_mission / C_game
constexpr int kMissionGame = 0x24;
constexpr int kMissionScene = 0x10;   // I3D_scene* the game renders each frame (found with `rstat`)
constexpr int kGamePlayer  = 0xE4;
constexpr int kGameScore = 0x3640, kGameScoreShown = 0x363E;   // the freeride's money (int), and whether the HUD shows it
// The shot queue (C_game::NewShoot fills it, the bullets' tick C_game::TickShoot moves and resolves it; both builds):
// a vector at +0x1D8 (begin, end +0x1DC) of 0x5C-byte records: +0x00 where from, +0x0C the way to where it ends (the
// static world's impact NewShoot found with testLine, or the weapon's reach), +0x1C where the bullet is now, +0x28 the
// damage, +0x2C the shooter, +0x34 the static primitive struck, +0x38 its point, +0x44 its normal, +0x50 (byte) 1
// when there is one, +0x58 (byte) its surface. The tick asks collRayTest only for the moving objects on the way; at
// the end of the way (|now - from|^2 + 1 >= |way|^2) it makes the static impact's effects.
constexpr int kGameShots = 0x1D8, kShotSize = 0x5C, kShotFrom = 0x00, kShotWay = 0x0C, kShotNow = 0x1C, kShotDamage = 0x28,
              kShotShooter = 0x2C, kShotPrim = 0x34, kShotHitPos = 0x38, kShotHitNormal = 0x44, kShotHits = 0x50, kShotSurface = 0x58;

template <class T> inline T &at(void *p, int off) { return *reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(p) + off); }

inline void *Mission() { return *reinterpret_cast<void **>(Exe().missionPtr); }
inline void *Collision() { return reinterpret_cast<void *>(Exe().collision); }
inline void *Game() { void *m = Mission(); return m ? at<void *>(m, kMissionGame) : nullptr; }
inline void *Player() { void *g = Game(); return g ? at<void *>(g, kGamePlayer) : nullptr; }

// All actors the game knows: the mission's scene actors (C_mission +0x38..+0x3C) and the temporary ones the
// traffic system adds (C_game +0x124..+0x128, see C_game::AddTemporaryActor 0x5A77C0).
inline void ListActors(std::vector<void *> *out) {
    void *m = Mission();
    if (!m) return;
    auto take = [&](void *vec) {
        void **b = at<void **>(vec, 0), **e = at<void **>(vec, 4);
        if (!b || e < b || e - b > 20000) return;
        for (void **p = b; p < e; p++) if (*p) out->push_back(*p);
    };
    take(reinterpret_cast<uint8_t *>(m) + 0x38);
    if (void *g = Game()) take(reinterpret_cast<uint8_t *>(g) + 0x124);
}

inline void *FindChild(void *frame, const char *name) {
    using Fn = void *(__stdcall *)(void *, const char *, uint32_t);
    void **vt = *reinterpret_cast<void ***>(frame);
    return reinterpret_cast<Fn>(vt[kVtFindChild / 4])(frame, name, 0xffff);
}

// I3D_frame::EnumFrames (vtable +0x34): stdcall(this, callback, ctx, flags, nameMask); the callback is
// stdcall(frame, ctx) and returns 1 to continue, 2 to stop (as LS3DF's own FindChildFrame does).
constexpr int kVtEnumFrames = 0x34;
inline int __stdcall CollectFrame(void *frame, void *ctx) {
    static_cast<std::vector<void *> *>(ctx)->push_back(frame);
    return 1;
}
// Direct children only; EnumFramesDeep walks the whole subtree (depth-first, parents before children).
inline void EnumFrames(void *root, std::vector<void *> *out, uint32_t flags = 0xffff) {
    using Fn = void(__stdcall *)(void *, int(__stdcall *)(void *, void *), void *, uint32_t, const char *);
    void **vt = *reinterpret_cast<void ***>(root);
    reinterpret_cast<Fn>(vt[kVtEnumFrames / 4])(root, CollectFrame, out, flags, nullptr);
}
inline void EnumFramesDeep(void *root, std::vector<void *> *out, int depth = 0) {
    if (depth > 32) return;
    std::vector<void *> kids;
    EnumFrames(root, &kids);
    for (void *k : kids) { out->push_back(k); EnumFramesDeep(k, out, depth + 1); }
}

inline const float *WorldMatrix(void *frame) {
    using Fn = const float *(__fastcall *)(void *);
    return reinterpret_cast<Fn>(Exe().frameWorldMatrix)(frame);
}

// Ray against the static world. Returns true on hit and fills hit point and normal.
inline bool TestLine(const V3 &from, const V3 &to, V3 *hit, V3 *normal) {
    using Fn = void *(__thiscall *)(void *, const V3 *, const V3 *, V3 *, V3 *, uint32_t);
    V3 delta = to - from, h{}, n{};
    void *r = reinterpret_cast<Fn>(Exe().testLine)(Collision(), &from, &delta, &h, &n, g_rayFlags);
    g_rayStats.rays++;
    if (r) {
        if (hit) *hit = h;
        if (normal) *normal = n;
        g_rayStats.hits++;
        g_rayStats.lastFrom = from; g_rayStats.lastTo = to; g_rayStats.lastHit = h; g_rayStats.lastNormal = n;
        g_rayStats.lastPrimFlags = *reinterpret_cast<uint32_t *>(r);
    }
    return r != nullptr;
}

// An actor's position in the world: its model frame's. (The actor position +0x24 of someone sitting in a car, Tommy
// at the wheel included, is in the car's frame, not the world's.)
// An actor's position (+0x24, a pedestrian's +0x10) and facing are kept in its model's parent's space: the game sets
// the model's local position from them. That is the world for a person in the street (the sector's matrix is the
// identity) and the tram's frame for one walking in a tram (the game parents him to it). World -> that space:
inline const float *ParentWorld(void *model) {
    void *parent = model ? at<void *>(model, kFrameParent) : nullptr;
    return parent ? WorldMatrix(parent) : nullptr;
}
inline V3 ToParentPoint(void *model, const V3 &world) {
    const float *pw = ParentWorld(model);
    return pw ? M3::Raw(pw).Inverse().Apply(world - V3{pw[12], pw[13], pw[14]}) : world;
}
inline V3 ToParentDir(void *model, const V3 &world) {
    const float *pw = ParentWorld(model);
    if (!pw) return world;
    M3 r = M3::FromWorld(pw);
    return V3{Dot(world, r.r[0]), Dot(world, r.r[1]), Dot(world, r.r[2])};
}
inline V3 FromParentDir(void *model, const V3 &local) {
    const float *pw = ParentWorld(model);
    return pw ? M3::FromWorld(pw).Apply(local) : local;
}

inline V3 ActorWorldPos(void *actor) {
    if (void *f = at<void *>(actor, kActorFrame)) {
        const float *w = WorldMatrix(f);
        if (w) return V3{w[12], w[13], w[14]};
    }
    return at<V3>(actor, kActorPos);
}

// The same ray, giving the primitive it hit: its first dword's low byte holds flags (0x40: it carries a parameter),
// its fourth byte the surface material (what the game's thrown objects sound like on it). Null: no hit.
inline const uint8_t *TestLinePrimitive(const V3 &from, const V3 &to, V3 *hit, V3 *normal) {
    using Fn = void *(__thiscall *)(void *, const V3 *, const V3 *, V3 *, V3 *, uint32_t);
    V3 delta = to - from, h{}, n{};
    void *r = reinterpret_cast<Fn>(Exe().testLine)(Collision(), &from, &delta, &h, &n, g_rayFlags);
    if (r) { if (hit) *hit = h; if (normal) *normal = n; }
    return static_cast<const uint8_t *>(r);
}

// ---- the mission
// The open mission's folder name under missions\ ("freeitaly", "mise01", "00menu" the main menu); "" when none.
inline const char *MissionName() {
    void *m = Mission();
    const char *name = m && Exe().missionName ? at<const char *>(m, Exe().missionName) : nullptr;
    return name ? name : "";
}

// ---- the car. A C_car is an actor of type 4; its rigid body, a C_Vehicle, is inside it (C_car + Exe().vehicle).
// The fields' offsets differ between the builds: they come from game_build.h (each read in the game's code and
// seen in the running 1.2 game). Positions are metres, the world's; +y is up.
constexpr int kActorTypeCar = 4;
constexpr int kHumanVehicle = 0x98;   // C_human: the vehicle he is in (a car, a tram), or null

// The car the player sits in now; null on foot (or in a tram).
inline void *PlayerCar() {
    void *p = Player();
    void *v = p ? at<void *>(p, kHumanVehicle) : nullptr;
    return v && at<int>(v, kActorType) == kActorTypeCar ? v : nullptr;
}
// The car the player drives or drove last (the one the HUD's speedometer is for); null before the first one.
inline void *LastCar() { return Exe().hudCar ? at<void *>(reinterpret_cast<void *>(Exe().hud), Exe().hudCar) : nullptr; }

// A C_Vehicle's flags (Car::Flags; read in Move's rest test and Tick's end, both builds).
constexpr uint32_t kVehAwake = 1, kVehCalm = 2, kVehAirborne = 8;

struct Car {
    void *car;
    explicit Car(void *c) : car(c) {}
    explicit operator bool() const { return car != nullptr; }
    template <class T> T &v(int off) const { return at<T>(car, Exe().vehicle + off); }

    V3 Position() const { return v<V3>(Exe().vehPos); }
    V3 Velocity() const { return v<V3>(Exe().vehVelocity); }           // m/s, the world's axes
    float Speed() const { return v<float>(Exe().vehSpeed); }           // m/s (the speedometer shows it in mph)
    float SpeedKmh() const { return Speed() * 3.6f; }
    float SteerAngle() const { return v<float>(Exe().vehSteer); }      // the wheels now: radians, left negative
    float SteerWanted() const { return v<float>(Exe().vehSteerWanted); }   // what the driver asks for
    float SteerMax() const { return v<float>(Exe().vehSteerMax); }     // the limit at this speed
    float Steering() const { float m = SteerMax(); return m > 1e-4f ? SteerAngle() / m : 0.0f; }   // -1 left .. 1 right
    int Gear() const { return v<int>(Exe().vehGear); }                 // -1 reverse, 0 neutral, 1...
    int GearMax() const { return v<int>(Exe().vehGearMax); }
    float Rpm() const { return EngineOn() ? v<float>(Exe().vehRps) * 60.0f : 0.0f; }   // as C_Vehicle::GetRotations
    float Fuel() const { return v<float>(Exe().vehFuel); }             // litres
    float FuelTank() const { return v<float>(Exe().vehFuelTank); }
    bool EngineOn() const { return v<uint8_t>(Exe().vehEngineOn) != 0; }
    // The engine's health, 1 whole .. 0 dead (crashes and bullets lower it; the engine stops at 0).
    float EngineHealth() const { float m = v<float>(Exe().vehEngineHealthMax); return m > 1e-4f ? v<float>(Exe().vehEngineHealth) / m : 0.0f; }
    float Damage() const { return 1.0f - EngineHealth(); }
    void *Frame() const { return at<void *>(car, kActorFrame); }       // its model: WorldMatrix(Frame()) for its axes

    // The rigid body (knowledge/notes/vehicle_physics.md). The physics' own axes and angular velocity: what
    // C_Vehicle::Move integrates each substep (the model frame follows them).
    void *Vehicle() const { return &at<uint8_t>(car, Exe().vehicle); }   // C_Vehicle*: what Tick, Engine, Move take
    V3 AngularVelocity() const { return v<V3>(Exe().vehTurn); }        // rad/s
    V3 Forward() const { return v<V3>(Exe().vehAxes); }                // the body's axes, unit vectors in the world
    V3 Right() const { return v<V3>(Exe().vehAxes + 12); }
    V3 Up() const { return v<V3>(Exe().vehAxes + 24); }
    // Its flags (kVehAwake...): a calm car is one Move zeroed (slower than 0.01 m/s and 0.015 rad/s); Tick lets it
    // sleep (clears awake) after 1 s calm or 4 s within 0.15 m.
    uint32_t Flags() const { return v<uint32_t>(Exe().vehicleFlags); }
    bool Awake() const { return (Flags() & kVehAwake) != 0; }
    bool Calm() const { return (Flags() & kVehCalm) != 0; }
    float StepTime() const { return v<float>(Exe().vehicleStepTime); }   // s: the last step Engine made (its dt)
    float SubstepMax() const { return v<float>(Exe().vehSubstepMax); }   // s: Engine cuts its dt into substeps this long at most
    int WheelCount() const { return v<int>(Exe().vehWheelCount); }
    void *Wheel(int i) const {                                         // an S_wheel (the kWheel* fields), or null
        void **w = v<void **>(Exe().vehWheels);
        return w && i >= 0 && i < WheelCount() ? w[i] : nullptr;
    }
};

// A car's wheel: S_wheel, 0x1D4 bytes, the same layout in both builds (C_Vehicle::DoWheelsCollision's suspension
// reads and writes the suspension's fields with the same code in 1.2 and 1.0, checked by tools/check_builds.py; the
// frames and the load were seen in both builds' games, the core's `car` lists them). Floats unless said. The suspension force is (free length - compression) * stiffness plus the damper (the compression's speed
// times kWheelDamper or the next float, by direction), in N; a wheel whose force is not positive leaves the ground.
constexpr int kWheelSize = 0x1D4;
// I3D_frame*: the wheel ("<model>.WHL0") and its dummy ("<model>.DWHL0": UpdateSteeringWheels writes its rotation)
constexpr int kWheelFrame = 0x04, kWheelSteerFrame = 0x08;
constexpr int kWheelLoad = 0x38;                             // N: the weight on it (a quarter of the taxi's, standing)
constexpr int kWheelStiffness = 0x88;                        // N/m (68000 for the taxi)
constexpr int kWheelFreeLength = 0x8C;                       // m (0.2)
constexpr int kWheelForce = 0x90;                            // N: the suspension's force this substep
constexpr int kWheelCompression = 0x94;                      // m: the last compression
constexpr int kWheelDamper = 0x98;                           // N s/m (1400; +0x9C 2600 the other way)
constexpr int kWheelFlags = 0x120;                           // uint32: kWheelOnGround...
constexpr uint32_t kWheelHandbrake = 4, kWheelOnGround = 8, kWheelTouched = 0x40;

// A car repaired as the scripts' car_repair does: set on its wheels on the ground, its body undented, its engine
// whole (C_car::RepairPosition(car, true)). Game thread only.
inline void CarRepair(void *car) {
    using Fn = void(__thiscall *)(void *, bool);
    if (car && Exe().carRepair) reinterpret_cast<Fn>(Exe().carRepair)(car, true);
}

// A car's state (position, rotation, speeds, engine, gear, fuel, damage and dents, wheels, lights, doors) saved and
// put back with the game's own functions, the ones its saved games use for every car. Its size is not the model's:
// it grows with the body's dented vertices (the same taxi: 1237 bytes whole, 1613 after touching a curb). The game's
// load finds each part by its name, writes only the vertices the state lists and leaves the others as they are, so
// the car is repaired first (CarRepair): a state with fewer dents than the car now has then comes back whole. A
// state goes back only into a car built as the one it came from (CarStateFits; false otherwise): another model's
// parts would be skipped by name, its wheels and seats by count. Game thread only. The player may sit in the car.
inline size_t CarStateSize(void *car) {
    using Fn = int(__thiscall *)(void *);
    return car && Exe().vehStateSize ? static_cast<size_t>(reinterpret_cast<Fn>(Exe().vehStateSize)(&at<uint8_t>(car, Exe().vehicle))) : 0;
}
inline bool CarStateSave(void *car, std::vector<uint8_t> *out) {
    using Fn = char(__thiscall *)(void *, void *);
    size_t n = CarStateSize(car);
    if (n < 8 || n > (16u << 20)) return false;
    out->assign(n, 0);
    return reinterpret_cast<Fn>(Exe().vehStateSave)(&at<uint8_t>(car, Exe().vehicle), out->data()) != 0;
}
// Whether a state was made for a car built as this one: the counts the game's load walks by, u32s at +352 (lights),
// +356 (body parts), +360 (seats), +364 (wheels), compared with the car's own state now (saving it only reads the
// car). Two cars of one model have the same; the frames' names do not tell (every car's is its own).
inline bool CarStateFits(void *car, const std::vector<uint8_t> &state) {
    std::vector<uint8_t> now;
    return state.size() >= 368 && CarStateSave(car, &now) && now.size() >= 368 && !memcmp(state.data() + 352, now.data() + 352, 16);
}
inline bool CarStateLoad(void *car, const std::vector<uint8_t> &state) {
    using Fn = char(__thiscall *)(void *, const void *);
    // {u32 9 (the format), u32 its size}: what the game checks is the first; the second keeps a cut buffer out
    if (!car || state.size() < 8 || *reinterpret_cast<const uint32_t *>(state.data()) != 9 ||
        *reinterpret_cast<const uint32_t *>(state.data() + 4) != state.size() || !CarStateFits(car, state))
        return false;
    CarRepair(car);
    return reinterpret_cast<Fn>(Exe().vehStateLoad)(&at<uint8_t>(car, Exe().vehicle), state.data()) != 0;
}

inline void SetLocalRotation(void *frame, const Q &q) {
    float *r = &at<float>(frame, kFrameRot);
    r[0] = q.w; r[1] = q.x; r[2] = q.y; r[3] = q.z;
    uint32_t &f = at<uint32_t>(frame, kFrameFlags);
    f &= ~(kFlagScaled | kFlagLocalBuilt | kFlagWorldValid | kFlagBoundValid);
    f |= kFlagRotValid | kFlagConstructed;
}

inline void SetLocalPosition(void *frame, const V3 &p) {
    float *m = &at<float>(frame, kFrameLocal);
    m[12] = p.x; m[13] = p.y; m[14] = p.z;
    uint32_t &f = at<uint32_t>(frame, kFrameFlags);
    f &= ~(kFlagLocalBuilt | kFlagWorldValid | kFlagBoundValid);
}

} // namespace game
