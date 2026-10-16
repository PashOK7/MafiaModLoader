// Props: models placed in the world without being actors (mml.h prop_create, prop_place, prop_attach, prop_show,
// prop_remove, actor_frame, frame_world; docs/MODDING.md "Props").
//
// A prop is an LS3D model frame made as world.cpp makes an actor's (the I3D driver's CreateFrame, the model file
// opened into it by the game's model cache) with no actor on it: placed in the world (linked into the sector at its
// place, as a spawned actor's frame is) or attached to any frame (a person's bone, a car's model or one of its
// parts, another prop: the frame's LinkTo), its local position, rotation and scale written as the game's own frame
// setters write them (SetPos; SetRot's quaternion +0x9C and flags; SetScale +0x90) and the frame updated. Shown and
// hidden by the frame's SetOn (vtable +0x24), removed by LinkTo(null) and Release. The host keeps every prop: a mod's
// go with its unload, every prop with the mission; a prop attached to an actor that is destroyed is put into the
// world where it was (the actor's destructor's hook, before the game's).
//
// A prop's own animation is played as the scripts' MODEL_PLAYANIM plays one (the animation cache's set given to the
// model's SetAnimation, the model put on the mission's list of animated models, which its tick steps), stopped as
// MODEL_STOPANIM stops it. A prop in the world is made a physical object as CREATE_PHYSICALOBJECT makes one (a
// C_public_physics on its frame: a box from its bounds in the collision's dynamic objects, falling, pushed, struck):
// the actor holds a reference of its own to the frame (the frame's count +4 raised, its byte +0x6C set: its
// destructor releases it), so the prop and the actor may go in either order. Taken out (gameRemoveTemp), the actor
// is deleted at the game's next tick; physics asked for again before that is made at the frame after its
// destructor (a prop moved while physical: taken out, moved, made again).
//
// Bridge methods (game thread): props.list, prop.create, prop.place, prop.attach, prop.show, prop.remove, prop.anim,
// prop.physics, prop.push.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

enum { kFrameModel = 9 };
enum { kVtRelease = 0x00, kVtUpdate = 0x18, kVtSetOn = 0x24, kVtSetName = 0x28, kVtLinkTo = 0x2C };
constexpr int kFrameScaleOff = 0x90, kFrameRotOff = 0x9C;

using CreateFrameFn = void *(__stdcall *)(void *driver, int type);
using OpenFn = int(__fastcall *)(uintptr_t cache, void *edx, void *frame, const char *file, int, int, int, int);
using ReleaseFn = int(__stdcall *)(void *frame);
using SetNameFn = int(__stdcall *)(void *frame, const char *name);
using UpdateFn = int(__stdcall *)(void *frame);
using SetOnFn = void(__stdcall *)(void *frame, bool on);
using LinkToFn = int(__stdcall *)(void *frame, void *parent, uint32_t flags);
using SetPosFn = void(__fastcall *)(void *frame, void *edx, const V3 *pos);
using SceneMovedFn = int(__fastcall *)(void *scene, void *edx, void *frame);

enum { kVtSetAnimation = 0x60, kVtStopAnimation = 0x68 };
constexpr int kFrameRefs = 0x04, kModelAnims = 0x150;   // (the frame's reference count; the model's animation channels)
constexpr int kChanSet = 0x08, kChanOn = 0x0C, kChanMode = 0x10, kChanTime = 0x14, kChanWeight = 0x20;
constexpr int kModeLoop = 2, kModeOnce = 1;
// C_actor's placement, which its GameDone gives the frame back when the actor goes (both builds, read in
// InternalPlacementReset and GameDone): the frame relinked to +0x58 (its parent at GameInit), its local position
// +0x3C and rotation +0x48 (w, x, y, z) written back.
constexpr int kActorOwnsFrame = 0x6C, kActorPlacePos = 0x3C, kActorPlaceRot = 0x48, kActorPlaceParent = 0x58;
constexpr int kFrameLocal = 0x50;
enum { kVtActorRelease = 0x44, kVtActorInit = 0x48 };

struct Prop {
    void *frame; MmlMod *mod; std::string model; void *parent; bool on;
    std::string anim; bool animListed = false; bool loop = false;   // (animListed: on the mission's list)
    void *phys = nullptr;      // its physical object (an actor), while it lives
    void *physGone = nullptr;  // one taken out, not destroyed yet
    bool want = false; MmlPropPhysics wantP{};   // physics to make once physGone is destroyed
    MmlPropPhysics physP{};
};
std::vector<Prop> g_props;
unsigned g_made;

const game::ExeBuild &E() { return game::Exe(); }
bool Known() { return game::g_exe && E().modelCache && E().modelOpen && E().i3dDriver && E().frameSetPos && E().sceneMoved; }
bool AnimKnown() { return game::g_exe && E().animCache && E().animOpen && E().missionAddAnim && E().missionDelAnim; }
bool PhysKnown() {
    return game::g_exe && E().physCtor && E().physInitBox && E().physImpulse && E().physSize && E().gameNew && E().gameAddTemp && E().gameRemoveTemp;
}

using AnimOpenFn = int(__fastcall *)(uintptr_t cache, void *edx, const char *file, void **set, int, int);
using SetAnimationFn = int(__stdcall *)(void *model, void *set, int channel, float weight, int mode);
using StopAnimationFn = int(__stdcall *)(void *model, int channel);
using SetLengthFn = int(__stdcall *)(void *set);
using SetReleaseFn = int(__stdcall *)(void *set);
using AddAnimFn = int(__fastcall *)(void *mission, void *edx, void *model, int cleared);
using DelAnimFn = void(__fastcall *)(void *mission, void *edx, void *model);
using GameNewFn = void *(__cdecl *)(size_t);
using ThisFn = void *(__fastcall *)(void *self, void *edx);
using ThisPtrFn = int(__fastcall *)(void *self, void *edx, void *arg);
using ImpulseFn = int(__fastcall *)(void *self, void *edx, const V3 *impulse, const V3 *at);
void *Vt(void *obj, int off) { return (*reinterpret_cast<void ***>(obj))[off / 4]; }

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

Prop *Find(void *frame) {
    for (Prop &p : g_props) if (p.frame == frame) return &p;
    return nullptr;
}

// ---- the math: LS3D's row vectors (world = local x parent's world), a rotation's rows its axes
void Mul(const float *a, const float *b, float *o) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}
bool InverseAffine(const float *m, float *o) {
    M3 r = M3::Raw(m), inv = r.Inverse();
    float det = Dot(r.r[0], Cross(r.r[1], r.r[2]));
    if (std::fabs(det) < 1e-12f) return false;
    V3 t{m[12], m[13], m[14]}, ti = -inv.Apply(t);
    for (int i = 0; i < 3; i++) { o[i * 4] = inv.r[i].x; o[i * 4 + 1] = inv.r[i].y; o[i * 4 + 2] = inv.r[i].z; o[i * 4 + 3] = 0; }
    o[12] = ti.x; o[13] = ti.y; o[14] = ti.z; o[15] = 1;
    return true;
}
// A 4x4 into position, rotation and scale (per axis).
void Decompose(const float *m, V3 *pos, Q *rot, V3 *scale) {
    V3 r[3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    float s[3];
    for (int i = 0; i < 3; i++) { s[i] = Len(r[i]); if (s[i] < 1e-9f) s[i] = 1; r[i] = r[i] * (1.0f / s[i]); }
    *pos = {m[12], m[13], m[14]};
    *scale = {s[0], s[1], s[2]};
    *rot = QuatOf(M3{{r[0], r[1], r[2]}});
}

// The frame's local transform written as the game's setters write it, then the frame updated.
void WriteLocal(void *f, const V3 *pos, const Q *rot, const V3 *scale) {
    if (rot) {
        float n = std::sqrt(rot->w * rot->w + rot->x * rot->x + rot->y * rot->y + rot->z * rot->z);
        if (n > 1e-9f) game::SetLocalRotation(f, Q{rot->w / n, rot->x / n, rot->y / n, rot->z / n});   // (SetRot's flags)
    }
    if (scale) {
        float *s = &game::at<float>(f, kFrameScaleOff);
        s[0] = scale->x; s[1] = scale->y; s[2] = scale->z;
        uint32_t &fl = game::at<uint32_t>(f, game::kFrameFlags);
        fl = (fl & 0xBFFFFEC3u) | 0x40000008u;   // (SetScale's, with the rotation valid: it was written)
    }
    if (pos) reinterpret_cast<SetPosFn>(E().frameSetPos)(f, nullptr, pos);
    reinterpret_cast<UpdateFn>(Vt(f, kVtUpdate))(f);
}

void *MissionScene() {
    void *m = game::Mission();
    return m ? game::at<void *>(m, game::kMissionScene) : nullptr;
}
// Into the world: linked into the sector at its place (as a spawned actor's frame is).
void IntoWorld(void *f) { reinterpret_cast<SceneMovedFn>(E().sceneMoved)(MissionScene(), nullptr, f); }

bool InChain(void *frame, void *of) {
    for (int i = 0; frame && i < 64; i++, frame = game::at<void *>(frame, game::kFrameParent)) if (frame == of) return true;
    return false;
}

std::string HexOf(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

} // namespace

void *PropCreate(MmlMod *mod, const char *model, void *parent, const float *pos, const float *rot, float scale, std::string *err) {
    if (!Known()) { *err = "making props is not known for this build"; return nullptr; }
    if (!game::Mission() || !MissionScene()) { *err = "no mission loaded"; return nullptr; }
    if (!model || !*model) { *err = "model: a file of the game's models (\"2tommy.i3d\", \"crate.i3d\"...)"; return nullptr; }
    std::string file = model;
    if (file.find('.') == std::string::npos) file += ".i3d";
    void *driver = *reinterpret_cast<void **>(E().i3dDriver);
    if (!driver) { *err = "no I3D driver"; return nullptr; }
    void *frame = nullptr;
    int opened = -1;
    auto open = [&] {
        frame = reinterpret_cast<CreateFrameFn>(Vt(driver, E().drvCreateFrame))(driver, kFrameModel);
        if (frame) opened = reinterpret_cast<OpenFn>(E().modelOpen)(E().modelCache, nullptr, frame, file.c_str(), 0, 0, 0, 0);
    };
    if (Guarded(open)) { *err = "opening the model faulted"; return nullptr; }
    if (!frame) { *err = "the driver made no frame"; return nullptr; }
    if (opened != 0) {
        auto release = [&] { reinterpret_cast<ReleaseFn>(Vt(frame, kVtRelease))(frame); };
        Guarded(release);
        *err = "no model \"" + file + "\" (the game's models\\ folder and archives, or a mod's files\\models\\)";
        return nullptr;
    }
    std::string base = file.substr(0, file.rfind('.'));
    size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    std::string name = base + "_prop" + std::to_string(++g_made);
    V3 p = pos ? V3{pos[0], pos[1], pos[2]} : V3{0, 0, 0};
    Q q = rot ? Q{rot[0], rot[1], rot[2], rot[3]} : Q{1, 0, 0, 0};
    V3 s{scale > 0 ? scale : 1, scale > 0 ? scale : 1, scale > 0 ? scale : 1};
    auto place = [&] {
        reinterpret_cast<SetNameFn>(Vt(frame, kVtSetName))(frame, name.c_str());
        if (parent) reinterpret_cast<LinkToFn>(Vt(frame, kVtLinkTo))(frame, parent, 0);
        WriteLocal(frame, &p, &q, &s);
        if (!parent) IntoWorld(frame);
    };
    if (Guarded(place)) { *err = "placing it faulted (the parent is not a frame?)"; return nullptr; }
    g_props.push_back({frame, mod, file, parent, true});
    return frame;
}

bool PropPhysics(void *prop, const MmlPropPhysics *p, std::string *err);
namespace {
void SavePlacement(void *actor, void *frame, bool unlink);
}

bool PropPlace(void *prop, const float *pos, const float *rot, float scale, std::string *err) {
    Prop *pr = Find(prop);
    if (!pr) { *err = "not a prop (props.list)"; return false; }
    V3 p = pos ? V3{pos[0], pos[1], pos[2]} : V3{};
    Q q = rot ? Q{rot[0], rot[1], rot[2], rot[3]} : Q{};
    V3 s{scale, scale, scale};
    // (a physical prop keeps its place in its actor: taken out, moved, made again, at rest)
    bool physical = pr->phys || pr->want;
    MmlPropPhysics again = pr->want ? pr->wantP : pr->physP;
    for (float &v : again.velocity) v = 0;
    if (physical) PropPhysics(prop, nullptr, err);
    auto f = [&] {
        WriteLocal(prop, pos ? &p : nullptr, rot ? &q : nullptr, scale > 0 ? &s : nullptr);
        if (!pr->parent && pos) IntoWorld(prop);   // (it may have moved into another room)
    };
    if (Guarded(f)) { *err = "placing it faulted"; return false; }
    if (pr->physGone) SavePlacement(pr->physGone, prop, false);   // (where the actor going leaves it: here)
    if (physical && !PropPhysics(prop, &again, err)) return false;
    return true;
}

bool PropAttach(void *prop, void *parent, bool keepWorld, std::string *err) {
    Prop *pr = Find(prop);
    if (!pr) { *err = "not a prop (props.list)"; return false; }
    if (parent && InChain(parent, prop)) { *err = "the parent is the prop or below it"; return false; }
    if (parent && (pr->phys || pr->want)) PropPhysics(prop, nullptr, err);   // (only a prop in the world is physical)
    auto f = [&] {
        float w[16], pw[16], pinv[16], local[16];
        memcpy(w, game::WorldMatrix(prop), sizeof w);
        if (parent) {
            memcpy(pw, game::WorldMatrix(parent), sizeof pw);
            if (!InverseAffine(pw, pinv)) { *err = "the parent's matrix cannot be inverted"; return; }
            Mul(w, pinv, local);
        } else {
            memcpy(local, w, sizeof local);
        }
        reinterpret_cast<LinkToFn>(Vt(prop, kVtLinkTo))(prop, parent, 0);
        if (keepWorld) {
            V3 p, s;
            Q q;
            Decompose(local, &p, &q, &s);
            WriteLocal(prop, &p, &q, &s);
        } else {
            reinterpret_cast<UpdateFn>(Vt(prop, kVtUpdate))(prop);
        }
        if (!parent) IntoWorld(prop);
        pr->parent = parent;
    };
    if (Guarded(f)) { *err = "attaching it faulted (the parent is not a frame?)"; return false; }
    return err->empty();
}

bool PropShow(void *prop, bool on, std::string *err) {
    Prop *pr = Find(prop);
    if (!pr) { *err = "not a prop (props.list)"; return false; }
    auto f = [&] { reinterpret_cast<SetOnFn>(Vt(prop, kVtSetOn))(prop, on); };
    if (Guarded(f)) { *err = "SetOn faulted"; return false; }
    pr->on = on;
    return true;
}

// ---- its animation (the scripts' MODEL_PLAYANIM, MODEL_STOPANIM)
int PropAnimPlay(void *prop, const char *anim, uint32_t flags, float weight, std::string *err) {
    Prop *pr = Find(prop);
    if (!pr) { *err = "not a prop (props.list)"; return -1; }
    if (!AnimKnown()) { *err = "a model's animations are not known for this build"; return -1; }
    void *mission = game::Mission();
    if (!mission) { *err = "no mission loaded"; return -1; }
    if (!anim || !*anim) { *err = "anim: a file of anims\\ (\"walk1.i3d\"...)"; return -1; }
    std::string file = anim;
    if (file.find('.') == std::string::npos) file += ".i3d";
    void *set = nullptr;
    int length = 0;
    auto f = [&] {
        reinterpret_cast<AnimOpenFn>(E().animOpen)(E().animCache, nullptr, file.c_str(), &set, 0, 0);
        if (!set) return;
        length = reinterpret_cast<SetLengthFn>(Vt(set, 0x08))(set);
        reinterpret_cast<SetAnimationFn>(Vt(prop, kVtSetAnimation))(prop, set, 0, weight > 0 ? weight : 1.0f,
                                                                    (flags & MML_PROP_ANIM_LOOP) ? kModeLoop : kModeOnce);
        if (!pr->animListed) reinterpret_cast<AddAnimFn>(E().missionAddAnim)(mission, nullptr, prop, 1);
        pr->animListed = true;
        reinterpret_cast<SetReleaseFn>(Vt(set, 0))(set);   // (the model took a reference of its own)
    };
    if (Guarded(f)) { *err = "playing it faulted"; return -1; }
    if (!set) { *err = "no animation \"" + file + "\" (anims\\ in the archives, or a mod's files\\anims\\)"; return 0; }
    pr->anim = file;
    pr->loop = (flags & MML_PROP_ANIM_LOOP) != 0;
    return length > 0 ? length : 1;
}

namespace {
void AnimUnlist(Prop &p) {
    if (!p.animListed) return;
    p.animListed = false;
    void *mission = game::Mission();
    auto f = [&] {
        reinterpret_cast<StopAnimationFn>(Vt(p.frame, kVtStopAnimation))(p.frame, 0);
        if (mission) reinterpret_cast<DelAnimFn>(E().missionDelAnim)(mission, nullptr, p.frame);
    };
    Guarded(f);
}
} // namespace

bool PropAnimStop(void *prop, std::string *err) {
    Prop *pr = Find(prop);
    if (!pr) { *err = "not a prop (props.list)"; return false; }
    AnimUnlist(*pr);
    pr->anim.clear();
    return true;
}

int PropAnimState(void *prop, MmlPropAnim *out) {
    Prop *pr = Find(prop);
    if (!pr) return 0;
    MmlPropAnim a{};
    snprintf(a.anim, sizeof a.anim, "%s", pr->anim.c_str());
    a.loop = pr->loop ? 1 : 0;
    auto f = [&] {
        uint8_t *ch = game::at<uint8_t *>(prop, kModelAnims);
        if (!ch || pr->anim.empty()) return;
        void *set = *reinterpret_cast<void **>(ch + kChanSet);
        if (!set) return;
        a.playing = ch[kChanOn] ? 1 : 0;
        a.time_ms = *reinterpret_cast<int *>(ch + kChanTime);
        a.weight = *reinterpret_cast<float *>(ch + kChanWeight);
        a.length_ms = reinterpret_cast<SetLengthFn>(Vt(set, 0x08))(set);
    };
    Guarded(f);
    if (out) *out = a;
    return 1;
}

// ---- its physics (the scripts' CREATE_PHYSICALOBJECT, DESTROY_PHYSICALOBJECT)
namespace {
void *MakePhysics(void *frame, const MmlPropPhysics &p, std::string *err) {
    void *phys = nullptr;
    bool inited = false;
    auto f = [&] {
        void *mem = reinterpret_cast<GameNewFn>(E().gameNew)(static_cast<size_t>(E().physSize));
        if (!mem) return;
        phys = reinterpret_cast<ThisFn>(E().physCtor)(mem, nullptr);
        inited = (reinterpret_cast<ThisPtrFn>(Vt(phys, kVtActorInit))(phys, nullptr, frame) & 0xFF) != 0;
        if (!inited) {
            reinterpret_cast<ThisFn>(Vt(phys, kVtActorRelease))(phys, nullptr);
            return;
        }
        float block[5] = {p.mass > 1 ? p.mass : 1.0f, p.velocity[0], p.velocity[1], p.velocity[2], 0.45f};
        reinterpret_cast<ThisPtrFn>(E().physInitBox)(phys, nullptr, block);
        game::at<int>(frame, kFrameRefs)++;            // (the actor's own reference, released by its destructor)
        game::at<uint8_t>(phys, kActorOwnsFrame) = 1;
        reinterpret_cast<ThisPtrFn>(E().gameAddTemp)(game::Game(), nullptr, phys);
    };
    if (Guarded(f)) { *err = "making the physical object faulted"; return nullptr; }
    if (!phys) { *err = "the game made no physical object"; return nullptr; }
    if (!inited) { *err = "the physical object refused the frame (another actor's?)"; return nullptr; }
    return phys;
}
// What the actor's GameDone gives the frame when it goes: where the frame is now (in its parent now), or, for a prop
// being removed, no parent at all.
void SavePlacement(void *actor, void *frame, bool unlink) {
    auto f = [&] {
        V3 pos, scale;
        Q rot;
        Decompose(reinterpret_cast<const float *>(reinterpret_cast<uint8_t *>(frame) + kFrameLocal), &pos, &rot, &scale);
        game::at<V3>(actor, kActorPlacePos) = pos;
        game::at<Q>(actor, kActorPlaceRot) = rot;
        game::at<void *>(actor, kActorPlaceParent) = unlink ? nullptr : game::at<void *>(frame, game::kFrameParent);
    };
    Guarded(f);
}
void TakePhysicsOut(Prop &p) {
    if (!p.phys) return;
    void *phys = p.phys;
    SavePlacement(phys, p.frame, false);   // (it stays where it lies)
    auto f = [&] { reinterpret_cast<ThisPtrFn>(E().gameRemoveTemp)(game::Game(), nullptr, phys); };
    Guarded(f);
    p.physGone = phys;   // (deleted at the game's next tick: PropsActorGone)
    p.phys = nullptr;
}
} // namespace

bool PropPhysics(void *prop, const MmlPropPhysics *p, std::string *err) {
    Prop *pr = Find(prop);
    if (!pr) { *err = "not a prop (props.list)"; return false; }
    if (!PhysKnown()) { *err = "physical objects are not known for this build"; return false; }
    if (!p) {
        pr->want = false;
        TakePhysicsOut(*pr);
        return true;
    }
    if (pr->parent) { *err = "the prop is attached to a frame: only a prop in the world can be physical"; return false; }
    if (!game::Mission() || !game::Game()) { *err = "no mission loaded"; return false; }
    TakePhysicsOut(*pr);
    if (pr->physGone) {   // (the one before is still there: made once it is destroyed)
        pr->want = true;
        pr->wantP = *p;
        return true;
    }
    void *phys = MakePhysics(prop, *p, err);
    if (!phys) return false;
    pr->phys = phys;
    pr->physP = *p;
    return true;
}

bool PropPush(void *prop, const float impulse[3], const float *at, std::string *err) {
    Prop *pr = Find(prop);
    if (!pr) { *err = "not a prop (props.list)"; return false; }
    if (!pr->phys) { *err = pr->want ? "its physics is made at the next frame" : "the prop is not physical (prop_physics)"; return false; }
    V3 i{impulse[0], impulse[1], impulse[2]}, a;
    if (at) {
        a = {at[0], at[1], at[2]};
    } else {
        float w[16];
        if (!FrameWorld(prop, w)) { *err = "no place"; return false; }
        a = {w[12], w[13], w[14]};
    }
    auto f = [&] { reinterpret_cast<ImpulseFn>(E().physImpulse)(pr->phys, nullptr, &i, &a); };
    if (Guarded(f)) { *err = "the impulse faulted"; return false; }
    return true;
}

void *PropBody(void *prop) {
    Prop *pr = Find(prop);
    return pr ? pr->phys : nullptr;
}

void PropsFrame() {
    for (Prop &p : g_props) {
        if (!p.want || p.physGone) continue;
        p.want = false;
        std::string err;
        void *phys = MakePhysics(p.frame, p.wantP, &err);
        if (phys) { p.phys = phys; p.physP = p.wantP; }
        else Log(MML_LOG_WARN, "props", "%s: physics not made again: %s", p.model.c_str(), err.c_str());
    }
}

namespace {
void Unmake(Prop &p) {
    AnimUnlist(p);
    TakePhysicsOut(p);   // (the actor keeps its own reference until its destructor)
    if (p.physGone) SavePlacement(p.physGone, p.frame, true);   // (its GameDone must not link the frame back)
    void *frame = p.frame;
    auto f = [&] {
        reinterpret_cast<LinkToFn>(Vt(frame, kVtLinkTo))(frame, nullptr, 0);
        reinterpret_cast<ReleaseFn>(Vt(frame, kVtRelease))(frame);
    };
    Guarded(f);
}
} // namespace

bool PropRemove(void *prop, std::string *err) {
    auto it = std::find_if(g_props.begin(), g_props.end(), [&](const Prop &p) { return p.frame == prop; });
    if (it == g_props.end()) { *err = "not a prop (props.list)"; return false; }
    // (its children props first go into the world where they are)
    for (Prop &c : g_props)
        if (c.frame != prop && c.parent && InChain(c.parent, prop)) { std::string e; PropAttach(c.frame, nullptr, true, &e); }
    it = std::find_if(g_props.begin(), g_props.end(), [&](const Prop &p) { return p.frame == prop; });
    Unmake(*it);
    g_props.erase(it);
    return true;
}

void PropsModGone(MmlMod *mod) {
    std::vector<void *> mine;
    for (const Prop &p : g_props) if (p.mod == mod) mine.push_back(p.frame);
    std::string err;
    for (void *f : mine) PropRemove(f, &err);
}
void PropsMissionClose() {   // (before the mission's world goes: the props unlinked and released; the physical objects
                             // go with the mission's actors, each releasing its own reference)
    for (Prop &p : g_props) {
        if (p.phys) p.physGone = p.phys;   // (the mission deletes its temporary actors itself)
        p.phys = nullptr;
        Unmake(p);          // (off the mission's list of animated models too)
    }
    g_props.clear();
}
void PropsActorGone(void *actor) {   // (its destructor, before the game's: props on it go into the world)
    bool ours = false;
    for (Prop &p : g_props) {
        if (p.physGone == actor) { p.physGone = nullptr; ours = true; }
        if (p.phys == actor) { p.phys = nullptr; ours = true; }   // (the game destroyed it: the prop stays where it lies)
    }
    if (ours) return;   // (its frame is the prop's: what hangs on it stays)
    void *model = game::at<void *>(actor, game::kActorFrame);
    if (!model) return;
    for (Prop &p : g_props)
        if (p.parent && InChain(p.parent, model)) {
            std::string err;
            PropAttach(p.frame, nullptr, true, &err);
        }
}

void *ActorFrame(void *actor) {
    void *f = nullptr;
    auto g = [&] {
        int t = game::at<int>(actor, game::kActorType);
        if (t > 0 && t < 64) f = game::at<void *>(actor, game::kActorFrame);
    };
    if (!actor || Guarded(g)) return nullptr;
    return f;
}
int FrameWorld(void *frame, float out[16]) {
    bool ok = false;
    auto g = [&] {
        const float *w = game::WorldMatrix(frame);
        if (w) { memcpy(out, w, 64); ok = true; }
    };
    if (!frame || !game::g_exe || Guarded(g)) return 0;
    return ok ? 1 : 0;
}

namespace {
bool IsVec(const json &v) { return v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number(); }
json R3(const float *v) { return json::array({std::round(v[0] * 1000) / 1000, std::round(v[1] * 1000) / 1000, std::round(v[2] * 1000) / 1000}); }

// A parent from the bridge: none, "world"; an actor ("player", "car", "0x...") and `bone` (a frame's name in its
// model: "r_hand", "neck"...); `frame`: any frame's address (another prop's too).
bool Parent(const json &p, void **out, std::string *err) {
    *out = nullptr;
    if (p.contains("frame") && p["frame"].is_string()) {
        *out = reinterpret_cast<void *>(ParseAddress(p["frame"].get<std::string>()));
        if (!*out) { *err = "frame: an address"; return false; }
        return true;
    }
    std::string a = p.value("parent", "world");
    if (a == "world" || a.empty()) return true;
    void *actor = nullptr;
    if (!ParseActor(a, &actor, err)) { *err = "parent: " + *err; return false; }
    void *model = ActorFrame(actor);
    if (!model) { *err = "parent: the actor has no model"; return false; }
    if (p.contains("bone") && p["bone"].is_string()) {
        std::string b = p["bone"].get<std::string>();
        void *f = nullptr;
        auto g = [&] { f = game::FindChild(model, b.c_str()); };
        Guarded(g);
        if (!f) { *err = "bone: no frame \"" + b + "\" in its model"; return false; }
        *out = f;
    } else {
        *out = model;
    }
    return true;
}

// pos/rot/dir/scale from the bridge: `rot` [w, x, y, z]; or `dir` [x, y, z] (where the prop's z axis points; y up).
bool Pose(const json &p, float *pos, bool *havePos, float *rot, bool *haveRot, float *scale) {
    *havePos = *haveRot = false;
    if (p.contains("pos") && IsVec(p["pos"])) { for (int i = 0; i < 3; i++) pos[i] = p["pos"][i].get<float>(); *havePos = true; }
    if (p.contains("rot") && p["rot"].is_array() && p["rot"].size() == 4) { for (int i = 0; i < 4; i++) rot[i] = p["rot"][i].get<float>(); *haveRot = true; }
    else if (p.contains("dir") && IsVec(p["dir"])) {
        V3 z = Norm({p["dir"][0].get<float>(), p["dir"][1].get<float>(), p["dir"][2].get<float>()}, {0, 0, 1});
        V3 up{0, 1, 0};
        if (std::fabs(Dot(z, up)) > 0.99f) up = {1, 0, 0};
        V3 x = Norm(Cross(up, z), {1, 0, 0}), y = Cross(z, x);
        Q q = QuatOf(M3{{x, y, z}});
        rot[0] = q.w; rot[1] = q.x; rot[2] = q.y; rot[3] = q.z;
        *haveRot = true;
    }
    *scale = p.value("scale", 0.0f);
    return true;
}

json PropJson(const Prop &p) {
    json j = {{"prop", HexOf(p.frame)}, {"model", p.model}, {"by", p.mod ? ModIdOf(p.mod) : "bridge"}, {"on", p.on},
              {"parent", p.parent ? HexOf(p.parent) : "world"}};
    float w[16];
    if (FrameWorld(p.frame, w)) {
        j["pos"] = R3(w + 12);
        j["axes"] = {R3(w), R3(w + 4), R3(w + 8)};
    }
    auto name = [&](void *f) -> std::string {
        std::string s;
        auto g = [&] { const char *n = game::at<const char *>(f, game::kFrameName); if (n) s.assign(n, strnlen(n, 64)); };
        Guarded(g);
        return s;
    };
    if (p.parent) j["parent_name"] = name(p.parent);
    j["name"] = name(p.frame);
    if (!p.anim.empty()) {
        MmlPropAnim a{};
        PropAnimState(p.frame, &a);
        j["anim"] = {{"file", a.anim}, {"playing", a.playing != 0}, {"loop", a.loop != 0}, {"ms", a.time_ms}, {"length_ms", a.length_ms},
                     {"weight", a.weight}};
    }
    if (p.phys) j["physics"] = {{"actor", HexOf(p.phys)}, {"mass", p.physP.mass > 1 ? p.physP.mass : 1.0f}};
    else if (p.want) j["physics"] = {{"pending", true}};
    return j;
}
} // namespace

bool PropsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "props.list" && method != "prop.create" && method != "prop.place" && method != "prop.attach" && method != "prop.show" &&
        method != "prop.remove" && method != "prop.anim" && method != "prop.physics" && method != "prop.push")
        return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "props.list") {
            json list = json::array();
            for (const Prop &pr : g_props) list.push_back(PropJson(pr));
            *reply = dump({{"known", Known()}, {"props", list}});
            *ok = true;
            return true;
        }
        if (method == "prop.remove" && p.value("all", false)) {
            std::vector<void *> all;
            for (const Prop &pr : g_props) all.push_back(pr.frame);
            for (void *f : all) PropRemove(f, &err);
            *reply = dump({{"removed", all.size()}});
            *ok = true;
            return true;
        }
        void *prop = nullptr;
        if (method != "prop.create") {
            prop = reinterpret_cast<void *>(ParseAddress(p.value("prop", "")));
            if (!Find(prop)) { *reply = method + ": prop: not a prop (props.list)"; return true; }
        }
        float pos[3] = {0, 0, 0}, rot[4] = {1, 0, 0, 0}, scale = 0;
        bool havePos, haveRot;
        Pose(p, pos, &havePos, rot, &haveRot, &scale);
        if (method == "prop.create") {
            void *parent = nullptr;
            if (!Parent(p, &parent, &err)) { *reply = "prop.create: " + err; return true; }
            if (!parent && !havePos) {   // `at`: a point or an actor and `ahead` metres in front of it, at its feet
                json at = p.contains("at") ? p["at"] : json("player");
                if (IsVec(at)) { for (int i = 0; i < 3; i++) pos[i] = at[i].get<float>(); }
                else {
                    void *a = nullptr;
                    if (!ParseActor(at.is_string() ? at.get<std::string>() : "player", &a, &err)) { *reply = "prop.create: at: " + err; return true; }
                    float w[16];
                    if (!FrameWorld(ActorFrame(a), w)) { *reply = "prop.create: at: the actor has no model"; return true; }
                    float ahead = p.value("ahead", 2.0f);
                    for (int i = 0; i < 3; i++) pos[i] = w[12 + i] + w[8 + i] * ahead;
                    pos[1] += p.value("up", 1.0f);
                }
            }
            void *made = PropCreate(nullptr, p.value("model", "").c_str(), parent, pos, haveRot ? rot : nullptr, scale, &err);
            if (!made) { *reply = "prop.create: " + err; return true; }
            *reply = dump(PropJson(*Find(made)));
        } else if (method == "prop.place") {
            if (!PropPlace(prop, havePos ? pos : nullptr, haveRot ? rot : nullptr, scale, &err)) { *reply = "prop.place: " + err; return true; }
            *reply = dump(PropJson(*Find(prop)));
        } else if (method == "prop.attach") {
            void *parent = nullptr;
            if (!Parent(p, &parent, &err)) { *reply = "prop.attach: " + err; return true; }
            if (!PropAttach(prop, parent, p.value("keep", true), &err)) { *reply = "prop.attach: " + err; return true; }
            if (!p.value("keep", true) && (havePos || haveRot || scale > 0)) PropPlace(prop, havePos ? pos : nullptr, haveRot ? rot : nullptr, scale, &err);
            *reply = dump(PropJson(*Find(prop)));
        } else if (method == "prop.show") {
            if (!PropShow(prop, p.value("on", true), &err)) { *reply = "prop.show: " + err; return true; }
            *reply = dump(PropJson(*Find(prop)));
        } else if (method == "prop.anim") {
            if (p.value("stop", false)) {
                if (!PropAnimStop(prop, &err)) { *reply = "prop.anim: " + err; return true; }
            } else {
                std::string a = p.value("play", "");
                if (a.empty()) { *reply = "prop.anim: play: an animation of anims\\ (\"walk1\"), or stop: true"; return true; }
                int n = PropAnimPlay(prop, a.c_str(), p.value("loop", false) ? MML_PROP_ANIM_LOOP : 0, p.value("weight", 1.0f), &err);
                if (n <= 0) { *reply = "prop.anim: " + err; return true; }
            }
            *reply = dump(PropJson(*Find(prop)));
        } else if (method == "prop.physics") {
            if (!p.value("on", true)) {
                if (!PropPhysics(prop, nullptr, &err)) { *reply = "prop.physics: " + err; return true; }
            } else {
                MmlPropPhysics ph{};
                ph.mass = p.value("mass", 10.0f);
                if (p.contains("velocity") && IsVec(p["velocity"]))
                    for (int i = 0; i < 3; i++) ph.velocity[i] = p["velocity"][i].get<float>();
                if (!PropPhysics(prop, &ph, &err)) { *reply = "prop.physics: " + err; return true; }
            }
            *reply = dump(PropJson(*Find(prop)));
        } else if (method == "prop.push") {
            if (!p.contains("impulse") || !IsVec(p["impulse"])) { *reply = "prop.push: impulse: [x, y, z] (kg m/s)"; return true; }
            float imp[3], at[3];
            for (int i = 0; i < 3; i++) imp[i] = p["impulse"][i].get<float>();
            bool haveAt = p.contains("at") && IsVec(p["at"]);
            if (haveAt) for (int i = 0; i < 3; i++) at[i] = p["at"][i].get<float>();
            if (!PropPush(prop, imp, haveAt ? at : nullptr, &err)) { *reply = "prop.push: " + err; return true; }
            *reply = dump(PropJson(*Find(prop)));
        } else {
            if (!PropRemove(prop, &err)) { *reply = "prop.remove: " + err; return true; }
            *reply = dump({{"removed", 1}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
