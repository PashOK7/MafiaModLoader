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
// game's own animation has the bones again), or the bones as they are. Before it plays he is put where the body lies (the pelvis over
// the ground) and turned the way it lies: at both animations' first frame the head lies along the model's z, behind
// him from the back (-z), ahead from the front (+z) (mafia-ragdoll's getup measured it). Then, for blend_ms, the host
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
// MML_EVENT_PEDESTRIAN_HIT: the element's place +0x10 (the world's), its health +0x15C (below 0: dead), its model
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
    void *frames[MML_BONE_COUNT];
    M3 rot[kBones];    // the body's world rotations at the hand-over
    V3 pelvis;         // and the pelvis's world place
    DWORD start;
    int ms;
};
std::vector<Blend> g_blends;
unsigned g_getUps;

// the parent of each bone among the skeleton (-1: the model's root), in mml.h's order
const int kParent[kBones] = {-1, 0, 1, 2, 3, 3, 5, 6, 7, 3, 9, 10, 11, 0, 13, 14, 0, 16, 17};

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

// one blend's frame: false when it is over or the person gone
bool BlendStep(Blend &b, DWORD now) {
    if (!HumanPosable(b.human)) return false;
    float t = b.ms > 0 ? static_cast<float>(now - b.start) / b.ms : 1.0f;
    if (t >= 1.0f) return false;
    t = t * t * (3 - 2 * t);   // (smooth at both ends)
    auto f = [&] {
        M3 anim[kBones];
        for (int i = 0; i < kBones; i++) anim[i] = b.frames[i] ? M3::FromWorld(game::WorldMatrix(b.frames[i])) : M3::Identity();
        const float *rootW = game::WorldMatrix(b.frames[MML_BONE_ROOT]);
        M3 root = M3::FromWorld(rootW);
        for (int i = 0; i < kBones; i++) {
            if (!b.frames[i]) continue;
            int p = kParent[i];
            const M3 &pb = p < 0 ? root : b.rot[p], &pa = p < 0 ? root : anim[p];
            Q from = QuatOf(b.rot[i] * pb.T()), to = QuatOf(anim[i] * pa.T());
            game::SetLocalRotation(b.frames[i], Slerp(from, to, t));
        }
        // the pelvis's place, from the body's to the animation's, in the root's space
        const float *pw = game::WorldMatrix(b.frames[MML_BONE_PELVIS]);
        V3 animPos{pw[12], pw[13], pw[14]};
        V3 at = b.pelvis * (1 - t) + animPos * t;
        V3 local = M3::Raw(rootW).Inverse().Apply(at - V3{rootW[12], rootW[13], rootW[14]});
        game::SetLocalPosition(b.frames[MML_BONE_PELVIS], local);
        BoneSet(b.frames[MML_BONE_PELVIS], nullptr, nullptr);   // (the model marked for its update after the mods)
    };
    return !Guarded(f);
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
    h.shape = HitsShapeOf(element);
    h.was_alive = h.alive = before > 0;
    auto rp = [&] { const V3 &p = game::at<V3>(element, kPedPos); h.pos = {p.x, p.y, p.z}; };
    Guarded(rp);
    // the mods' filters first (hits.cpp), unless pedBulletHit has filtered this bullet already
    bool taken = !HitsInPedBullet(element) && HitsFilter(element, 1, &h);
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

int HumanGetUp(void *human, int blendMs, std::string *err) {
    bool ped = !IsPerson(human) && PedestrianKnown(human);
    if (!ped && !IsPerson(human)) { *err = "neither a person (humans_list) nor a traffic pedestrian (pedestrians_list)"; return 0; }
    if (!ped && (!E().humanActs || !E().humanState)) { *err = "the action queue is not known for this build"; return 0; }
    if (ped && (!E().pedAnimStart || !E().collGridAdd || !E().collGridMove)) { *err = "a pedestrian's get-up is not known for this build"; return 0; }
    Blend b{};
    b.human = human;
    if (!HumanBones(human, b.frames) || !b.frames[MML_BONE_ROOT] || !b.frames[MML_BONE_NECK]) { *err = "no skeleton"; return 0; }
    bool alive = false;
    auto ra = [&] { alive = ped ? game::at<float>(human, kPedHealth) > 0 : game::at<uint8_t>(human, game::kActorAlive) != 0; };
    if (Guarded(ra) || !alive) { *err = "dead (a body does not get up)"; return 0; }
    float w[MML_BONE_COUNT][16] = {};
    if (!PoseOf(human, b.frames, w)) { *err = "his bones could not be read"; return 0; }
    int lying = Lying(w);
    bool back = lying >= 0;
    auto f = [&] {
        for (int i = 0; i < kBones; i++) b.rot[i] = b.frames[i] ? M3::FromWorld(w[i]) : M3::Identity();
        const float *pw = w[MML_BONE_PELVIS], *nw = w[MML_BONE_NECK];
        b.pelvis = {pw[12], pw[13], pw[14]};
        V3 head = Norm(V3{nw[12] - pw[12], 0, nw[14] - pw[14]}, {0, 0, 1});
        V3 facing = back ? -head : head;
        // the place: the pelvis over the ground
        V3 at = b.pelvis;
        MmlHit hit{};
        hit.size = sizeof hit;
        const float from[3] = {at.x, at.y + 1.0f, at.z}, to[3] = {at.x, at.y - 3.0f, at.z};
        if (WorldRay(from, to, MML_WORLD_STATIC, ped ? nullptr : human, &hit)) at.y = hit.pos[1];
        if (ped) {
            PedestrianGetUp(human, at, facing, back ? kGetUpBack : kGetUpFront);
            return;
        }
        void *model = game::at<void *>(human, game::kActorFrame);
        game::at<V3>(human, game::kActorPos) = game::ToParentPoint(model, at);
        game::at<V3>(human, game::kActorFacing) = game::ToParentDir(model, facing);
        // the queue: the get-up, then its end; run from its start
        uint8_t *q = static_cast<uint8_t *>(human) + E().humanActs;
        game::at<int>(q, 0x00) = 1;
        game::at<int>(q, 0x04) = back ? kGetUpBack : kGetUpFront;
        game::at<uint8_t>(q, 0x0C) = 0;
        game::at<int>(q, 0x10) = 0;
        game::at<int>(q, 0x14) = 3;
        game::at<int>(human, E().humanActIndex) = 0;
        game::at<int>(human, E().humanState) = 6;
        game::at<int>(human, kAnimPlaying) = 0;   // (the game plays nothing new when it is the one playing)
    };
    if (Guarded(f)) { *err = "faulted while handing him back"; return 0; }
    g_blends.erase(std::remove_if(g_blends.begin(), g_blends.end(), [&](const Blend &x) { return x.human == human; }), g_blends.end());
    b.start = GetTickCount();
    b.ms = std::min(std::max(blendMs, 0), 10000);
    if (b.ms > 0) g_blends.push_back(b);
    g_getUps++;
    return back ? 1 : 2;
}

void AnimsBeforeRender() {
    if (g_blends.empty()) return;
    DWORD now = GetTickCount();
    for (size_t i = 0; i < g_blends.size();)
        if (BlendStep(g_blends[i], now)) i++;
        else g_blends.erase(g_blends.begin() + i);
}

void AnimsModGone(MmlMod *mod) {
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
void AnimsMissionClose() { g_replaced.clear(); g_blends.clear(); }

bool AnimsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "human.anim" && method != "human.anim_state" && method != "human.get_up" && method != "human.anims") return false;
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
        if (p.contains("pedestrian") && (method == "human.anim_state" || method == "human.get_up")) {
            std::string e = p.value("pedestrian", "");
            h = reinterpret_cast<void *>(ParseAddress(e));
            if (!PedestrianKnown(h)) { *reply = method + ": pedestrian: " + e + " is not a traffic pedestrian (pedestrians.list)"; return true; }
        } else {
            if (a == "player") h = HumanPlayer();
            else if (!ParseActor(a, &h, &err)) { *reply = method + ": actor: " + err; return true; }
            if (!IsPerson(h)) { *reply = method + ": actor: not a person the loader knows (humans.list; \"player\")"; return true; }
        }
        if (method == "human.anim_state") {
            MmlHumanAnim s{};
            s.size = sizeof s;
            HumanAnimState(h, &s);
            *reply = dump({{"playing", s.playing}, {"asked", s.asked}, {"name", s.name ? s.name : ""}, {"state", s.state}, {"lying", s.lying},
                           {"time_ms", s.time_ms}, {"length_ms", s.length_ms},
                           {"blending", std::any_of(g_blends.begin(), g_blends.end(), [&](const Blend &b) { return b.human == h; })},
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
