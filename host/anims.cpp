// People's animations, getting up, and the traffic pedestrians' hits.
//
// Animations are played the game's own way (game_build.h humanPlayAnim, humanStopAnim, humanChangeAnim: what the
// scripts' PERSON_PLAYANIM, PERSON_STOPANIM and HUMAN_CHANGEANIM call): C_human::Do_PlayAnim hands a file of anims\ to
// the person's animations machine (a cross-fade from what he does; his state +0x40C becomes 4, and 1 again at its
// end), Do_StopAnim gives him his own at once, ChangeAnim plays one of his own animations as another. What he plays
// is +0x70 (the number of the game's table, game_build.h animTable), what is asked +0x74.
//
// Getting up (human_get_up): a body a mod posed is handed back as a knock-down hands it back, through the person's
// action queue (humanActs, 0x14 bytes an entry: {1 an animation, its number, ?, a byte, ?}, then {3 the end};
// humanActIndex 0; humanState 6; what Do_ShockDown writes). The get-up is the game's own: 111 "smrt pad end
// vstavani" from the back, 338 "x panika leh nahoru" from the front (the table's numbers, both builds), chosen by
// which way his front faces (right x spine, as the model's x x y is its z, the facing: up on the back). The pose is
// the one a mod or the bridge last drew (humans.cpp keeps it after each RENDER_BEGIN's update: between frames the
// game's own animation has the bones again), or the bones as they are. Before it plays he is put under the pelvis
// at the physical root's height and turned the way it lies: the first-frame head lies along the model's z, behind
// him from the back (-z), ahead from the front (+z). The host holds for 250 mission ms, fits the actual animation's
// pelvis horizontally and in yaw onto the physical pelvis, then for blend_ms
// draws each bone from the body's pose into the animation's, joint by joint: the body's world rotations taken at the
// hand-over, each bone's rotation relative to its parent blended (the pelvis's relative to the model's root as it is
// now), the pelvis's world place blended; written in RENDER_BEGIN after the game posed the animation.
//
// A traffic pedestrian gets up its own way (mafia-ragdoll's getup.cpp, read in both builds' pedHit and pedAnimStart):
// its element (pedestrians.cpp) keeps its place +0x10 and facing +0x1C (the world's), a sequence state +0x14C (3: two
// animations to play, 2: one, 1: playing the last, 0: walking), the animation to play +0x164, the one playing +0x160,
// the next +0x168, whether the playing one hands over to walking before its end +0x16C (+0x16D the next's). It is
// put where the body lies and turned as a person is, its model placed by its own SetWorldPos and SetDir (vtable +0x4,
// +0xC: what pedHit does) and updated (+0x18); out of the wait by a car it left (+0x1B0, +0x190, +0x1AC: a driver hit
// by his own car's door stood in it after getting up, untouchable); its collision object (+0xC4, its place +0x10:
// the model's (0, 0.9, 0)) put back into the grid at the body (collGridMove when it is in a cell, +0xC; else
// collGridAdd; +0x150 set); then the sequence at its last animation, the get-up, and pedAnimStart starts it (as a
// knock-down's get-up starts). The blend is the person's. human_anim_state of a pedestrian: its sequence state, the
// animation playing and asked, how it lies.
//
// Pedestrians: the traffic system's walkers are elements, not actors; their Hit (game_build.h pedHit,
// thiscall(element, type, dir, part, attacker, float damage) ret 0x14) is hooked and told as
// MML_EVENT_PEDESTRIAN_HIT: the element's place +0x10 (the world's; a bullet's hit: the point it struck), its health
// +0x15C (below 0: dead), its model
// frame the +4 of its collision object at +0xC4.
//
// Bridge methods (game thread): human.anim {actor, play | stop | replace, with, simple}, human.anim_state {actor |
// pedestrian}, human.get_up {actor | pedestrian, blend_ms}, human.anims {query} (the game's table, read from the game).
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kAnimPlaying = 0x70, kAnimAsked = 0x74;
constexpr int kGetUpBack = 111, kGetUpFront = 338;
constexpr int kPedPos = 0x10, kPedHealth = 0x15C, kPedColl = 0xC4;
constexpr int kPedDir = 0x1C, kPedModel = 0x40, kPedState = 0x14C, kPedInGrid = 0x150, kPedPlaying = 0x160, kPedAnim = 0x164,
              kPedNext = 0x168, kPedTimed = 0x16C, kPedNextTimed = 0x16D, kPedCarWaitMs = 0x190, kPedOwnCar = 0x1AC,
              kPedCarWait = 0x1B0, kPedCollPos = kPedColl + 0x10, kPedCollCell = kPedColl + 0x0C;
constexpr float kPedCollUp = 0.9f;
constexpr int kVtSetWorldPos = 0x04, kVtSetDir = 0x0C, kVtUpdate = 0x18;
constexpr int kBones = 19;   // pelvis..l_foot (the root is the 20th)

const game::ExeBuild &E() { return game::Exe(); }

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

// A pedestrian handed back where its body lies (at, the ground under the pelvis; facing as a person's), its get-up
// played by its own sequence (the comment at the top). Inside the caller's guard.
void PedestrianGetUp(void *element, const V3 &at, const V3 &facing, int anim) {
    uint8_t *p = static_cast<uint8_t *>(element);
    void *model = game::at<void *>(p, kPedModel);
    game::at<V3>(p, kPedPos) = at;
    game::at<V3>(p, kPedDir) = facing;
    void **vt = *reinterpret_cast<void ***>(model);
    V3 pos = at, dir = facing;
    reinterpret_cast<void(__stdcall *)(void *, const V3 *)>(vt[kVtSetWorldPos / 4])(model, &pos);
    reinterpret_cast<void(__stdcall *)(void *, const V3 *, int)>(vt[kVtSetDir / 4])(model, &dir, 0);
    reinterpret_cast<void(__stdcall *)(void *)>(vt[kVtUpdate / 4])(model);
    game::at<uint8_t>(p, kPedCarWait) = 0;
    game::at<int>(p, kPedCarWaitMs) = 0;
    game::at<uint32_t>(p, kPedOwnCar) = 0;
    game::at<V3>(p, kPedCollPos) = at + V3{0, kPedCollUp, 0};
    using GridFn = void(__fastcall *)(void *, void *, void *);
    reinterpret_cast<GridFn>(game::at<uint32_t>(p, kPedCollCell) ? E().collGridMove : E().collGridAdd)(game::Collision(), nullptr,
                                                                                                        p + kPedColl);
    game::at<uint8_t>(p, kPedInGrid) = 1;
    game::at<int>(p, kPedState) = 2;
    game::at<int>(p, kPedAnim) = anim;
    game::at<int>(p, kPedNext) = 0;
    game::at<uint8_t>(p, kPedTimed) = 1;
    game::at<uint8_t>(p, kPedNextTimed) = 0;
    game::at<int>(p, kPedPlaying) = -1;   // (its own get-up may be the one playing: started again from its start)
    reinterpret_cast<void(__fastcall *)(void *)>(E().pedAnimStart)(p);
}

using PlayFn = int(__fastcall *)(void *human, void *edx, const char *name, int simple, int load);
using StopFn = int(__fastcall *)(void *human, void *edx);
using ChangeFn = void *(__fastcall *)(void *human, void *edx, const char *own, const char *with);
using PedHitFn = int(__fastcall *)(void *element, void *edx, int type, const V3 *dir, int part, void *attacker, float damage);
PedHitFn oPedHit;
bool g_pedHooked;
unsigned g_pedHits;

struct Replaced { MmlMod *mod; void *human; std::string own; };
std::vector<Replaced> g_replaced;

struct Blend {
    void *human;
    MmlMod *owner;
    void *model;
    bool ped, back, fitted, switched;
    void *frames[MML_BONE_COUNT];
    int parent[kBones];
    M3 rot[kBones];    // the body's world rotations at the hand-over
    V3 pelvis;         // and the pelvis's world place
    int lying, elapsed, blendStart, ms;
    float fitError;
};
std::vector<Blend> g_blends;
unsigned g_getUps;

bool IsPerson(void *human) {
    if (!game::g_exe || !HumanKnown(human)) return false;
    uint32_t type = 0;
    auto f = [&] { type = game::at<uint32_t>(human, game::kActorType); };
    return !Guarded(f) && (type == 2 || type == 27);
}

std::string AnimFile(const char *name) {
    std::string s = name ? name : "";
    if (!s.empty() && s.find('.') == std::string::npos) s += ".i3d";
    return s;
}

const char *TableName(int number) {
    if (!E().animTable) return "";
    const char *n = "";
    auto f = [&] {
        for (int k = 0; k < E().animCount; k++) {
            const uint32_t *rec = reinterpret_cast<const uint32_t *>(E().animTable + 8 * k);
            if (static_cast<int>(rec[1]) == number) { n = reinterpret_cast<const char *>(rec[0]); return; }
        }
    };
    Guarded(f);
    return n;
}

// The pose to hand back: the one a mod (or the bridge) last drew, if it is recent, else the bones as they are now.
bool PoseOf(void *human, void *const *frames, float w[][16]) {
    if (HumanLastPose(human, w, 500)) return true;
    auto f = [&] {
        for (int i = 0; i < MML_BONE_COUNT; i++)
            if (frames[i]) memcpy(w[i], game::WorldMatrix(frames[i]), sizeof w[i]);
    };
    return !Guarded(f);
}
V3 At(const float *m) { return {m[12], m[13], m[14]}; }

// 1 on the back, -1 on the front, 0 neither: his front (the model's right x up, as its x x y is its z, the facing)
// from the shoulders, the hips and the spine
int Lying(float w[][16]) {
    V3 right = (At(w[MML_BONE_R_SHOULDER]) - At(w[MML_BONE_L_SHOULDER])) + (At(w[MML_BONE_R_THIGH]) - At(w[MML_BONE_L_THIGH]));
    V3 spine = At(w[MML_BONE_NECK]) - At(w[MML_BONE_PELVIS]);
    float up = Norm(Cross(right, spine), {0, 0, 0}).y;
    return up > 0.5f ? 1 : up < -0.5f ? -1 : 0;
}

Q Slerp(Q a, Q b, float t) {
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) { b = {-b.w, -b.x, -b.y, -b.z}; d = -d; }
    float wa = 1 - t, wb = t;
    if (d < 0.9995f) {
        float th = std::acos(d), s = std::sin(th);
        wa = std::sin((1 - t) * th) / s;
        wb = std::sin(t * th) / s;
    }
    Q q{a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb};
    float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return n > 1e-9f ? Q{q.w / n, q.x / n, q.y / n, q.z / n} : Q{1, 0, 0, 0};
}

V3 Yaw(const V3 &v, float angle) {
    float c = std::cos(angle), s = std::sin(angle);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
M3 Yaw(const M3 &r, float angle) { return {{Yaw(r.r[0], angle), Yaw(r.r[1], angle), Yaw(r.r[2], angle)}}; }

// Place both the game actor and its real root, in their parent's coordinates (including tram passengers).
void PlaceRoot(Blend &b, const M3 &worldRot, const V3 &worldPos) {
    int posOff = b.ped ? kPedPos : game::kActorPos, dirOff = b.ped ? kPedDir : game::kActorFacing;
    game::at<V3>(b.human, posOff) = game::ToParentPoint(b.model, worldPos);
    game::at<V3>(b.human, dirOff) = game::ToParentDir(b.model, worldRot.r[2]);
    void *root = b.frames[MML_BONE_ROOT];
    M3 rot = worldRot;
    V3 pos = worldPos;
    if (void *parent = game::at<void *>(root, game::kFrameParent)) {
        const float *w = game::WorldMatrix(parent);
        rot = rot * M3::FromWorld(w).T();
        pos = M3::Raw(w).Inverse().Apply(pos - At(w));
    }
    game::SetLocalRotation(root, QuatOf(rot));
    game::SetLocalPosition(root, pos);
    reinterpret_cast<void(__stdcall *)(void *)>((*reinterpret_cast<void ***>(b.model))[kVtUpdate / 4])(b.model);
}

void PlayGetUp(Blend &b) {
    int anim = b.back ? kGetUpBack : kGetUpFront;
    void *machine = b.ped ? game::at<void *>(b.human, 8) : static_cast<uint8_t *>(b.human) + E().humanAnimMachine;
    if (machine) {
        // The old fall's TCK ended somewhere else than the physical body. Do not cross-fade its obsolete
        // root trajectory into this get-up; the skeleton is blended from the explicit physics pose below.
        game::at<int>(machine, game::kAnimTrack) = -1;
        game::at<int>(machine, game::kAnimPreviousTrack) = -1;
    }
    if (b.ped) {
        PedestrianGetUp(b.human, At(game::WorldMatrix(b.frames[MML_BONE_ROOT])),
                        game::FromParentDir(b.model, game::at<V3>(b.human, kPedDir)), anim);
        return;
    }
    uint8_t *q = static_cast<uint8_t *>(b.human) + E().humanActs;
    game::at<int>(q, 0x00) = 1;
    game::at<int>(q, 0x04) = anim;
    game::at<uint8_t>(q, 0x0C) = 0;
    game::at<int>(q, 0x10) = 0;
    game::at<int>(q, 0x14) = 3;
    game::at<int>(b.human, E().humanActIndex) = 0;
    game::at<int>(b.human, E().humanState) = 6;
    game::at<int>(b.human, kAnimPlaying) = 0;
}

void FitGetUp(Blend &b) {
    const float *pw = game::WorldMatrix(b.frames[MML_BONE_PELVIS]);
    M3 anim = M3::FromWorld(pw);
    V3 pelvis = At(pw);
    float s = 0, c = 0;
    for (int i = 0; i < 3; i++) {
        s += Cross(anim.r[i], b.rot[MML_BONE_PELVIS].r[i]).y;
        c += anim.r[i].x * b.rot[MML_BONE_PELVIS].r[i].x + anim.r[i].z * b.rot[MML_BONE_PELVIS].r[i].z;
    }
    float turn = std::atan2(s, c);
    const float *rw = game::WorldMatrix(b.frames[MML_BONE_ROOT]);
    M3 root = M3::FromWorld(rw);
    // Native TCK Tick writes the actor/element position again on the next frame. Placing only the frame
    // fits for one render, then blends towards the stale trajectory and can jump several metres later.
    const float *oldLocal = &game::at<float>(b.frames[MML_BONE_ROOT], game::kFrameLocal);
    M3 oldRot = M3::FromWorld(oldLocal);
    V3 oldPos = At(oldLocal);
    V3 shift = b.pelvis - pelvis;
    shift.y = 0;
    PlaceRoot(b, Yaw(root, turn), pelvis + Yaw(At(rw) - pelvis, turn) + shift);
    void *machine = b.ped ? game::at<void *>(b.human, 8) : static_cast<uint8_t *>(b.human) + E().humanAnimMachine;
    if (machine && E().animRecompileMatrix) {
        const float *now = &game::at<float>(b.frames[MML_BONE_ROOT], game::kFrameLocal);
        M3 delta = oldRot.T() * M3::FromWorld(now);
        V3 offset = At(now) - delta.Apply(oldPos);
        float transform[16] = {delta.r[0].x,delta.r[0].y,delta.r[0].z,0,
                               delta.r[1].x,delta.r[1].y,delta.r[1].z,0,
                               delta.r[2].x,delta.r[2].y,delta.r[2].z,0,offset.x,offset.y,offset.z,1};
        using RebaseFn = void(__fastcall *)(void *, void *, const float *);
        reinterpret_cast<RebaseFn>(E().animRecompileMatrix)(machine, nullptr, transform);
        game::at<V3>(machine, game::kAnimTrackOffset) = delta.Apply(game::at<V3>(machine, game::kAnimTrackOffset));
        if (b.ped) {
            // pedTick's collision sweep starts at its saved pre-track position (+0x34).
            V3 &previous = game::at<V3>(b.human, 0x34);
            previous = delta.Apply(previous) + offset;
            V3 &travel = game::at<V3>(b.human, 0x174);
            travel = delta.Apply(travel);
        }
    }
    V3 error = At(game::WorldMatrix(b.frames[MML_BONE_PELVIS])) - b.pelvis;
    error.y = 0;
    b.fitError = Len(error);
    b.fitted = true;
    b.blendStart = b.elapsed;
    HLOG("getup %p: pelvis fitted (turn %.1f deg, shift %.3f m)", b.human, turn * 57.29578f, Len(shift));
}

// Hold the physical pose until the game's requested animation is real, fit that animation onto the body,
// then blend in joint-local space. Time advances with mission ticks, so pausing never completes a handover.
bool BlendStep(Blend &b) {
    if (!HumanPosable(b.human)) return false;
    bool keep = true;
    auto f = [&] {
        if (game::at<void *>(b.human, b.ped ? kPedModel : game::kActorFrame) != b.model ||
            (b.ped ? game::at<float>(b.human, kPedHealth) <= 0 : !game::at<uint8_t>(b.human, game::kActorAlive))) {
            keep = false;
            return;
        }
        int playing = game::at<int>(b.human, b.ped ? kPedPlaying : kAnimPlaying);
        if (!b.fitted && b.elapsed >= 250 && playing == (b.back ? kGetUpBack : kGetUpFront)) {
            float w[MML_BONE_COUNT][16] = {};
            for (int i = 0; i < kBones; i++) if (b.frames[i]) memcpy(w[i], game::WorldMatrix(b.frames[i]), sizeof w[i]);
            int side = Lying(w);
            if (!b.switched && side && b.lying && side != b.lying) {
                b.back = !b.back;
                b.switched = true;
                PlayGetUp(b);
                b.elapsed = 0;
            } else {
                FitGetUp(b);
            }
        }
        if (!b.fitted && b.elapsed > 2000) { keep = false; return; }
        float t = b.fitted ? (b.ms > 0 ? static_cast<float>(b.elapsed - b.blendStart) / b.ms : 1.0f) : 0.0f;
        if (t >= 1.0f) {
            // Keep the grid following until the native get-up is over, without posing the model any longer.
            keep = b.elapsed < 10000 && playing == (b.back ? kGetUpBack : kGetUpFront);
            return;
        }
        t = t * t * (3 - 2 * t);
        M3 anim[kBones];
        for (int i = 0; i < kBones; i++) anim[i] = b.frames[i] ? M3::FromWorld(game::WorldMatrix(b.frames[i])) : M3::Identity();
        const float *rootW = game::WorldMatrix(b.frames[MML_BONE_ROOT]);
        M3 root = M3::FromWorld(rootW);
        V3 animPos = At(game::WorldMatrix(b.frames[MML_BONE_PELVIS]));
        for (int i = 0; i < kBones; i++) {
            if (!b.frames[i]) continue;
            int p = b.parent[i];
            if (p == -2) {
                void *parent = game::at<void *>(b.frames[i], game::kFrameParent);
                M3 parentW = parent ? M3::FromWorld(game::WorldMatrix(parent)) : M3::Identity();
                Q rot = Slerp(QuatOf(b.rot[i]), QuatOf(anim[i]), t);
                float world[16];
                QuatMatrix(rot, world);
                game::SetLocalRotation(b.frames[i], QuatOf(M3::FromWorld(world) * parentW.T()));
                continue;
            }
            const M3 &pb = p < 0 ? root : b.rot[p], &pa = p < 0 ? root : anim[p];
            Q from = QuatOf(b.rot[i] * pb.T()), to = QuatOf(anim[i] * pa.T());
            game::SetLocalRotation(b.frames[i], Slerp(from, to, t));
        }
        // the pelvis's place, from the body's to the animation's, in the root's space
        V3 at = b.pelvis * (1 - t) + animPos * t;
        V3 local = M3::Raw(rootW).Inverse().Apply(at - V3{rootW[12], rootW[13], rootW[14]});
        game::SetLocalPosition(b.frames[MML_BONE_PELVIS], local);
        BoneSet(b.frames[MML_BONE_PELVIS], nullptr, nullptr);   // (the model marked for its update after the mods)
    };
    return !Guarded(f) && keep;
}

int __fastcall hkPedHit(void *element, void *edx, int type, const V3 *dir, int part, void *attacker, float damage) {
    float before = 0;
    auto rb = [&] { before = game::at<float>(element, kPedHealth); };
    Guarded(rb);
    MmlHumanHit h{};
    h.type = type;
    if (dir) h.dir = {dir->x, dir->y, dir->z};
    h.damage = damage;
    h.attacker = attacker;
    h.part = part;
    h.was_alive = h.alive = before > 0;
    auto rp = [&] { const V3 &p = game::at<V3>(element, kPedPos); h.pos = {p.x, p.y, p.z}; };
    Guarded(rp);
    // the mods' filters first (hits.cpp), unless pedBulletHit has filtered this bullet already
    bool taken = !HitsInPedBullet(element, &h) && HitsFilter(element, 1, &h);
    int r = 0;
    if (!taken) {
        V3 d{h.dir.x, h.dir.y, h.dir.z};
        r = oPedHit(element, edx, h.type, dir ? &d : nullptr, h.part, h.attacker, h.damage);
    }
    MmlEvent ev{};
    auto f = [&] {
        h.alive = game::at<float>(element, kPedHealth) > 0;
        void *coll = static_cast<uint8_t *>(element) + kPedColl;
        ev.other = game::at<void *>(coll, 4);
    };
    if (!Guarded(f)) {
        g_pedHits++;
        ev.size = sizeof ev;
        ev.type = MML_EVENT_PEDESTRIAN_HIT;
        ev.actor = element;
        ev.hit = &h;
        Dispatch(ev);
    }
    return r;
}

} // namespace

void AnimsInit() {
    if (!game::g_exe || !E().pedHit) return;
    std::string err;
    g_pedHooked = HookAdd(nullptr, E().pedHit, reinterpret_cast<void *>(&hkPedHit), reinterpret_cast<void **>(&oPedHit), &err);
    if (!g_pedHooked) HWARN("anims: the pedestrians' Hit is not hooked (no PEDESTRIAN_HIT): %s", err.c_str());
}

int HumanVehicleContact(void *human, void *vehicle, const float velocity[3], const float point[3]) {
    if (!game::g_exe || !human || !vehicle || !velocity || !point) return 0;
    bool ped = !IsPerson(human) && PedestrianKnown(human);
    if (!ped && !IsPerson(human)) return 0;
    int result = 0;
    auto f = [&] {
        V3 v{velocity[0], velocity[1], velocity[2]}, at{point[0], point[1], point[2]};
        if (!std::isfinite(Len(v)) || !std::isfinite(Len(at)) || Len(v) > 500) return;
        std::vector<void *> actors;
        game::ListActors(&actors);
        game::ListTrafficCars(&actors);
        const bool known = vehicle == game::PlayerCar() || vehicle == game::LastCar() ||
                           std::find(actors.begin(), actors.end(), vehicle) != actors.end();
        if (!known ||
            game::at<uint32_t>(vehicle, game::kActorType) != 4) return;
        if (ped) {
            if (!E().pedCarHit || game::at<float>(human, kPedHealth) <= 0 ||
                !game::at<uint8_t>(human, 0x0C) || !game::at<void *>(human, kPedModel)) return;
            // Native stdcall, ret 0x18 in both builds: car, unused, relative velocity, unused, unused, model.
            // This ends taxi waving and invokes pedHit with native strength/damage, rather than emulating it.
            using Fn = int(__stdcall *)(void *, int, const V3 *, int, int, void *);
            result = reinterpret_cast<Fn>(E().pedCarHit)(vehicle, 0, &v, 0, 0, game::at<void *>(human, kPedModel)) & 0xFF;
        } else {
            if (!game::at<uint8_t>(human, game::kActorAlive)) return;
            void *carFrame = game::at<void *>(vehicle, game::kActorFrame);
            void *frame = game::at<void *>(human, game::kActorFrame);
            for (int i = 0; frame && i < 24; i++, frame = game::at<void *>(frame, game::kFrameParent))
                if (frame == carFrame) return;
            MmlHumanHit hit{};
            hit.type = 8;
            hit.dir = {v.x, v.y, v.z};
            hit.pos = {at.x, at.y, at.z};
            hit.attacker = vehicle;
            std::string err;
            result = HumanHitCall(human, &hit, &err);
        }
    };
    return Guarded(f) ? 0 : result != 0;
}

int HumanAnimPlay(void *human, const char *anim, int flags, std::string *err) {
    if (!IsPerson(human)) { *err = "not a person the loader knows (humans_list)"; return -1; }
    std::string file = AnimFile(anim);
    if (file.empty() || file.size() > 60) { *err = "anim: a file of anims\\ (1..60 characters)"; return -1; }
    if (!E().humanPlayAnim) { *err = "not known for this build"; return -1; }
    int ms = -1;
    auto f = [&] { ms = reinterpret_cast<PlayFn>(E().humanPlayAnim)(human, nullptr, file.c_str(), (flags & MML_ANIM_SIMPLE) ? 1 : 0, 1); };
    if (Guarded(f)) { *err = "Do_PlayAnim faulted"; return -1; }
    return ms < 0 ? 0 : ms;
}

int HumanAnimStop(void *human, std::string *err) {
    if (!IsPerson(human)) { *err = "not a person the loader knows (humans_list)"; return 0; }
    if (!E().humanStopAnim) { *err = "not known for this build"; return 0; }
    auto f = [&] { reinterpret_cast<StopFn>(E().humanStopAnim)(human, nullptr); };
    if (Guarded(f)) { *err = "Do_StopAnim faulted"; return 0; }
    g_blends.erase(std::remove_if(g_blends.begin(), g_blends.end(), [&](const Blend &b) { return b.human == human; }), g_blends.end());
    return 1;
}

int HumanAnimReplace(MmlMod *mod, void *human, const char *own, const char *with, std::string *err) {
    if (!IsPerson(human)) { *err = "not a person the loader knows (humans_list)"; return 0; }
    std::string o = AnimFile(own), w = with && *with ? AnimFile(with) : "";
    if (o.empty() || o.size() > 60 || w.size() > 60) { *err = "own, with: files of anims\\ (1..60 characters)"; return 0; }
    if (!E().humanChangeAnim) { *err = "not known for this build"; return 0; }
    auto f = [&] { reinterpret_cast<ChangeFn>(E().humanChangeAnim)(human, nullptr, o.c_str(), w.empty() ? nullptr : w.c_str()); };
    if (Guarded(f)) { *err = "ChangeAnim faulted"; return 0; }
    g_replaced.erase(std::remove_if(g_replaced.begin(), g_replaced.end(), [&](const Replaced &r) { return r.human == human && r.own == o; }),
                     g_replaced.end());
    if (!w.empty()) g_replaced.push_back({mod, human, o});
    return 1;
}

int HumanAnimState(void *human, MmlHumanAnim *out) {
    bool ped = out && !IsPerson(human) && PedestrianKnown(human);
    if (!out || (!ped && !IsPerson(human))) return 0;
    uint32_t size = out->size;
    MmlHumanAnim a{};
    auto f = [&] {
        if (ped) {   // a pedestrian: its sequence (no animations machine of a C_human's)
            a.playing = game::at<int>(human, kPedPlaying);
            a.asked = game::at<int>(human, kPedAnim);
            a.state = game::at<int>(human, kPedState);
            return;
        }
        a.playing = game::at<int>(human, kAnimPlaying);
        a.asked = game::at<int>(human, kAnimAsked);
        a.state = E().humanState ? game::at<int>(human, E().humanState) : 0;
        if (E().animTime && E().humanAnimMachine) {
            using TimeFn = void(__fastcall *)(void *machine, void *edx, int *time, int *length);
            reinterpret_cast<TimeFn>(E().animTime)(static_cast<uint8_t *>(human) + E().humanAnimMachine, nullptr, &a.time_ms, &a.length_ms);
        }
    };
    if (Guarded(f)) return 0;
    // (the table's numbers above 511 are a set's variants of the same one: 513 of 1, 1025 of 1...)
    a.name = a.playing ? TableName(a.playing) : "";
    if (a.playing && !*a.name) a.name = TableName(a.playing & 0x1FF);
    void *frames[MML_BONE_COUNT];
    float w[MML_BONE_COUNT][16] = {};
    a.lying = HumanBones(human, frames) && PoseOf(human, frames, w) ? Lying(w) : 0;
    a.size = size;
    memcpy(out, &a, size < sizeof a ? size : sizeof a);
    return 1;
}

int HumanGetUp(void *human, int blendMs, std::string *err, MmlMod *owner, const MmlBone *pose, int count) {
    bool ped = !IsPerson(human) && PedestrianKnown(human);
    if (!ped && !IsPerson(human)) { *err = "neither a person (humans_list) nor a traffic pedestrian (pedestrians_list)"; return 0; }
    if (!ped && (!E().humanActs || !E().humanState)) { *err = "the action queue is not known for this build"; return 0; }
    if (ped && (!E().pedAnimStart || !E().collGridAdd || !E().collGridMove)) { *err = "a pedestrian's get-up is not known for this build"; return 0; }
    Blend b{};
    b.human = human;
    b.owner = owner;
    b.ped = ped;
    if (!HumanBones(human, b.frames) || !b.frames[MML_BONE_ROOT] || !b.frames[MML_BONE_NECK]) { *err = "no skeleton"; return 0; }
    bool alive = false;
    auto ra = [&] { alive = ped ? game::at<float>(human, kPedHealth) > 0 : game::at<uint8_t>(human, game::kActorAlive) != 0; };
    if (Guarded(ra) || !alive) { *err = "dead (a body does not get up)"; return 0; }
    // seated in a car (the car he uses, C_human +0x98, with his model hung under the car's): the game keeps him in
    // his seat; a get-up would put him on the street under the car still seated (it killed a driver posed in a crash)
    bool seated = false;
    auto rs = [&] {
        void *c = game::at<void *>(human, game::kHumanVehicle);
        if (!c || game::at<uint32_t>(c, game::kActorType) != 4) return;
        void *carFrame = game::at<void *>(c, game::kActorFrame);
        void *fr = game::at<void *>(human, game::kActorFrame);
        for (int i = 0; fr && i < 24; i++, fr = game::at<void *>(fr, game::kFrameParent)) if (fr == carFrame) { seated = true; return; }
    };
    if (!ped && (Guarded(rs) || seated)) { *err = "seated in a car (the game holds him in his seat: nothing to get up from)"; return 0; }
    float w[MML_BONE_COUNT][16] = {};
    if (pose) {
        if (count != MML_BONE_COUNT) { *err = "pose must have MML_BONE_COUNT entries"; return 0; }
        bool valid = true;
        auto readPose = [&] {
            for (int i = 0; i < MML_BONE_COUNT; i++) {
                if (pose[i].frame != b.frames[i]) { valid = false; return; }
                if (!b.frames[i]) continue;
                memcpy(w[i], pose[i].world, sizeof w[i]);
                for (float x : w[i]) if (!std::isfinite(x)) { valid = false; return; }
                M3 r = M3::Raw(w[i]);
                if (Len(r.r[0]) < 1e-5f || Len(r.r[1]) < 1e-5f || Len(r.r[2]) < 1e-5f ||
                    std::fabs(Dot(r.r[0], Cross(r.r[1], r.r[2]))) < 1e-5f) { valid = false; return; }
            }
        };
        if (Guarded(readPose) || !valid) { *err = "invalid physical pose or stale skeleton frames"; return 0; }
    } else if (!PoseOf(human, b.frames, w)) { *err = "his bones could not be read"; return 0; }
    int lying = Lying(w);
    bool back = lying >= 0;
    b.lying = lying;
    b.back = back;
    auto f = [&] {
        b.model = game::at<void *>(human, ped ? kPedModel : game::kActorFrame);
        for (int i = 0; i < kBones; i++) {
            b.rot[i] = b.frames[i] ? M3::FromWorld(w[i]) : M3::Identity();
            b.parent[i] = -2;
            if (!b.frames[i]) continue;
            void *parent = game::at<void *>(b.frames[i], game::kFrameParent);
            if (parent == b.frames[MML_BONE_ROOT]) b.parent[i] = -1;
            else for (int j = 0; j < kBones; j++) if (b.frames[j] && b.frames[j] == parent) { b.parent[i] = j; break; }
        }
        const float *pw = w[MML_BONE_PELVIS], *nw = w[MML_BONE_NECK];
        b.pelvis = {pw[12], pw[13], pw[14]};
        V3 head = b.rot[MML_BONE_PELVIS].r[1];
        head.y = 0;
        if (Len(head) < 0.3f) head = {nw[12] - pw[12], 0, nw[14] - pw[14]};
        head = Norm(head, {0, 0, 1});
        M3 root = M3::FromWorld(w[MML_BONE_ROOT]);
        // Authored head directions, measured against the actual get-ups in the reference mod.
        V3 initial = root.Apply(back ? V3{0.05f, 0, -0.97f} : V3{0.04f, 0, 1.0f});
        initial.y = 0;
        initial = Norm(initial, {0, 0, 1});
        float turn = std::atan2(Cross(initial, head).y, Dot(initial, head));
        PlaceRoot(b, Yaw(root, turn), {b.pelvis.x, w[MML_BONE_ROOT][13], b.pelvis.z});
        PlayGetUp(b);
    };
    if (Guarded(f)) { *err = "faulted while handing him back"; return 0; }
    g_blends.erase(std::remove_if(g_blends.begin(), g_blends.end(), [&](const Blend &x) { return x.human == human; }), g_blends.end());
    b.ms = std::min(std::max(blendMs, 0), 10000);
    g_blends.push_back(b);
    g_getUps++;
    return back ? 1 : 2;
}

void AnimsBeforeRender() {
    if (g_blends.empty()) return;
    for (size_t i = 0; i < g_blends.size();)
        if (BlendStep(g_blends[i])) i++;
        else g_blends.erase(g_blends.begin() + i);
}

void AnimsTick(int ms) {
    for (Blend &b : g_blends) {
        b.elapsed += std::clamp(ms, 0, 200);
        if (!HumanPosable(b.human)) continue;
        auto f = [&] {
            if (!b.ped) {
                if (b.back && game::at<int>(b.human, kAnimPlaying) == kGetUpBack &&
                    game::at<void *>(b.human, game::kActorFrame) == b.model)
                    game::at<float>(b.human, game::kHumanTrackLift) = 0.0f;
                return;
            }
            uint8_t *p = static_cast<uint8_t *>(b.human);
            if (game::at<void *>(p, kPedModel) != b.model || !game::at<uint32_t>(p, kPedCollCell)) return;
            const float *w = game::WorldMatrix(b.model);
            game::at<V3>(p, kPedCollPos) = At(w) + M3::Raw(w).Apply({0, kPedCollUp, 0});
            using GridFn = void(__fastcall *)(void *, void *, void *);
            reinterpret_cast<GridFn>(E().collGridMove)(game::Collision(), nullptr, p + kPedColl);
        };
        Guarded(f);
    }
}

int HumanGetUpCancel(MmlMod *owner, void *human) {
    size_t before = g_blends.size();
    g_blends.erase(std::remove_if(g_blends.begin(), g_blends.end(), [&](const Blend &b) {
        return b.human == human && b.owner == owner;
    }), g_blends.end());
    return before != g_blends.size();
}

void AnimsModGone(MmlMod *mod) {
    g_blends.erase(std::remove_if(g_blends.begin(), g_blends.end(), [&](const Blend &b) { return b.owner == mod; }), g_blends.end());
    for (size_t i = 0; i < g_replaced.size();) {
        Replaced r = g_replaced[i];
        if (r.mod != mod) { i++; continue; }
        g_replaced.erase(g_replaced.begin() + i);
        if (IsPerson(r.human) && E().humanChangeAnim) {
            auto f = [&] { reinterpret_cast<ChangeFn>(E().humanChangeAnim)(r.human, nullptr, r.own.c_str(), nullptr); };
            Guarded(f);
        }
    }
}
void AnimsActorGone(void *actor) {
    g_replaced.erase(std::remove_if(g_replaced.begin(), g_replaced.end(), [&](const Replaced &r) { return r.human == actor; }), g_replaced.end());
    g_blends.erase(std::remove_if(g_blends.begin(), g_blends.end(), [&](const Blend &b) { return b.human == actor; }), g_blends.end());
}
void AnimsBeforeLoad() { g_blends.clear(); }
void AnimsMissionClose() { g_replaced.clear(); AnimsBeforeLoad(); }

bool AnimsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "human.anim" && method != "human.anim_state" && method != "human.get_up" && method != "human.anims" &&
        method != "human.vehicle_contact") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "human.anims") {   // the game's animation table, read from the running game
            if (!game::g_exe || !E().animTable) { *reply = "human.anims: no game"; return true; }
            std::string q = p.value("query", "");
            for (char &c : q) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            json list = json::array();
            auto f = [&] {
                for (int k = 0; k < E().animCount; k++) {
                    const uint32_t *rec = reinterpret_cast<const uint32_t *>(E().animTable + 8 * k);
                    std::string n = reinterpret_cast<const char *>(rec[0]), l = n;
                    for (char &c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    if (q.empty() || l.find(q) != std::string::npos) list.push_back({{"number", rec[1]}, {"file", n}});
                }
            };
            Guarded(f);
            *reply = dump({{"count", list.size()}, {"anims", list}});
            *ok = true;
            return true;
        }
        void *h = nullptr;
        std::string err, a = p.value("actor", "player");
        if (p.contains("pedestrian") && (method == "human.anim_state" || method == "human.get_up" || method == "human.vehicle_contact")) {
            std::string e = p.value("pedestrian", "");
            h = reinterpret_cast<void *>(ParseAddress(e));
            if (!PedestrianKnown(h)) { *reply = method + ": pedestrian: " + e + " is not a traffic pedestrian (pedestrians.list)"; return true; }
        } else {
            if (a == "player") h = HumanPlayer();
            else if (!ParseActor(a, &h, &err)) { *reply = method + ": actor: " + err; return true; }
            if (!IsPerson(h)) { *reply = method + ": actor: not a person the loader knows (humans.list; \"player\")"; return true; }
        }
        if (method == "human.vehicle_contact") {
            void *car = nullptr;
            if (!ParseActor(p.value("car", "car"), &car, &err)) { *reply = "human.vehicle_contact: car: " + err; return true; }
            if (!p.contains("velocity") || !p["velocity"].is_array() || p["velocity"].size() != 3 ||
                !p.contains("pos") || !p["pos"].is_array() || p["pos"].size() != 3) {
                *reply = "human.vehicle_contact: velocity and pos must each contain three numbers"; return true;
            }
            float v[3], at[3];
            for (int i = 0; i < 3; i++) { v[i] = p["velocity"][i].get<float>(); at[i] = p["pos"][i].get<float>(); }
            if (!HumanVehicleContact(h, car, v, at)) { *reply = "human.vehicle_contact: refused target or contact"; return true; }
            *reply = dump({{"accepted", true}});
        } else if (method == "human.anim_state") {
            MmlHumanAnim s{};
            s.size = sizeof s;
            HumanAnimState(h, &s);
            json handover = nullptr;
            for (const Blend &b : g_blends) if (b.human == h) {
                handover = {{"elapsed_ms", b.elapsed}, {"fitted", b.fitted}, {"switched", b.switched},
                            {"fit_error_m", b.fitError}, {"from", b.back ? "back" : "front"}};
                break;
            }
            *reply = dump({{"playing", s.playing}, {"asked", s.asked}, {"name", s.name ? s.name : ""}, {"state", s.state}, {"lying", s.lying},
                           {"time_ms", s.time_ms}, {"length_ms", s.length_ms},
                           {"blending", std::any_of(g_blends.begin(), g_blends.end(), [&](const Blend &b) { return b.human == h; })},
                           {"handover", handover},
                           {"pedestrian_hits", g_pedHits}, {"pedestrians_hooked", g_pedHooked}});
        } else if (method == "human.get_up") {
            int r = HumanGetUp(h, p.value("blend_ms", 600), &err);
            if (!r) { *reply = "human.get_up: " + err; return true; }
            *reply = dump({{"from", r == 1 ? "back" : "front"}, {"animation", r == 1 ? kGetUpBack : kGetUpFront}});
        } else if (p.contains("play")) {
            int ms = HumanAnimPlay(h, p.value("play", "").c_str(), p.value("simple", false) ? MML_ANIM_SIMPLE : 0, &err);
            if (ms < 0) { *reply = "human.anim: " + err; return true; }
            *reply = dump({{"ms", ms}, {"note", ms ? "" : "length 0: no such animation file?"}});
        } else if (p.value("stop", false)) {
            if (!HumanAnimStop(h, &err)) { *reply = "human.anim: " + err; return true; }
            *reply = dump({{"stopped", true}});
        } else if (p.contains("replace")) {
            if (!HumanAnimReplace(nullptr, h, p.value("replace", "").c_str(), p.value("with", "").c_str(), &err)) { *reply = "human.anim: " + err; return true; }
            *reply = dump({{"replaced", p.value("replace", "")}, {"with", p.value("with", "")}});
        } else {
            *reply = "human.anim: play (a file of anims\\, \"simple\"), stop: true, or replace (his own) with (another; empty: his own again)";
            return true;
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
