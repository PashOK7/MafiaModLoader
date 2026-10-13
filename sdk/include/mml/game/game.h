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

// C_mission / C_game
constexpr int kMissionGame = 0x24;
constexpr int kMissionScene = 0x10;   // I3D_scene* the game renders each frame (found with `rstat`)
constexpr int kGamePlayer  = 0xE4;

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
