// People and their skeletons (mml.h humans_list, human_skeleton, bone_set; docs/MODDING.md "People, skeletons, poses").
//
// The host keeps every C_human the game constructs (game_hooks.cpp: its constructor and the actors' destructor),
// the player too. A person's skeleton is the 18 joint frames of the game's human models found by name under his model
// frame (actor + 0x68) by LS3D's own FindChildFrame (vtable +0x38), with the pelvis (the thighs' parent) and the
// model's root (the pelvis's parent): the names mafia-ragdoll found and drives. Each bone is an I3D_frame: its world
// matrix (+0x10), its local rotation (+0x9C, a quaternion w x y z) and position (+0x50's row 3).
//
// A pose is written into the bones' local rotation and position, their frames marked to be built again, and the
// frame's Update (vtable +0x18) called, as mafia-ragdoll writes its ragdoll's: done in RENDER_BEGIN (after the game's
// animation set the bones for this frame and before LS3D draws them), it is what is drawn. The bridge's human.pose
// holds a pose for a time (applied at every RENDER_BEGIN until then): an agent's test of a pose.
//
// A traffic pedestrian (pedestrians.cpp: an element, not a C_human) has the same skeleton under its model frame
// (element + 0x40): human_skeleton, bone_set and the poses take an element as they take a person; humans_list and
// HumanKnown stay the C_humans'.
//
// Bridge methods (game thread): humans.list, human.skeleton {actor | pedestrian}, human.pose {actor | pedestrian,
// bones, ms}.
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

std::vector<void *> g_humans;

const char *const kBoneNames[MML_BONE_COUNT] = {"pelvis", "back1", "back2", "back3", "neck",
                                                "r_shoulder", "r_arm", "r_elbow", "r_hand", "l_shoulder", "l_arm", "l_elbow", "l_hand",
                                                "r_thigh", "r_shin", "r_foot", "l_thigh", "l_shin", "l_foot", "root"};

std::string Hex(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

bool Readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return static_cast<const uint8_t *>(p) + n <= static_cast<const uint8_t *>(mi.BaseAddress) + mi.RegionSize;
}

bool Alive(void *h);
constexpr int kPedModel = 0x40;
void *ModelOf(void *human) {
    void *m = nullptr;
    int at = Alive(human) ? game::kActorFrame : PedestrianKnown(human) ? kPedModel : -1;
    if (at < 0) return nullptr;
    __try { m = game::at<void *>(human, at); } __except (EXCEPTION_EXECUTE_HANDLER) { m = nullptr; }
    return m;
}

// The skeleton's frames (null where a model has none of that name).
bool FindBones(void *human, void **out) {
    memset(out, 0, sizeof(void *) * MML_BONE_COUNT);
    void *model = ModelOf(human);
    if (!model || !Readable(model, 0x130)) return false;
    __try {
        for (int i = 0; i < MML_BONE_COUNT; i++) {
            if (i == MML_BONE_PELVIS || i == MML_BONE_ROOT) continue;
            out[i] = game::FindChild(model, kBoneNames[i]);
        }
        void *thigh = out[MML_BONE_R_THIGH] ? out[MML_BONE_R_THIGH] : out[MML_BONE_L_THIGH];
        out[MML_BONE_PELVIS] = thigh ? game::at<void *>(thigh, game::kFrameParent) : nullptr;
        out[MML_BONE_ROOT] = out[MML_BONE_PELVIS] ? game::at<void *>(out[MML_BONE_PELVIS], game::kFrameParent) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return out[MML_BONE_PELVIS] != nullptr;
}

bool ReadBone(void *frame, MmlBone *b) {
    __try {
        const float *w = game::WorldMatrix(frame);
        if (w) memcpy(b->world, w, sizeof b->world);
        const float *q = &game::at<float>(frame, game::kFrameRot);
        memcpy(b->rot, q, sizeof b->rot);
        const float *l = &game::at<float>(frame, game::kFrameLocal);
        b->pos[0] = l[12]; b->pos[1] = l[13]; b->pos[2] = l[14];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The bone's local rotation and position (its frame marked to be built again); the model is updated after (Flush).
bool WriteBone(void *frame, const float *rot, const float *pos) {
    __try {
        if (rot) {
            float n = std::sqrt(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3]);
            if (!(n > 1e-6f)) return false;
            game::SetLocalRotation(frame, Q{rot[0] / n, rot[1] / n, rot[2] / n, rot[3] / n});
        }
        if (pos) game::SetLocalPosition(frame, V3{pos[0], pos[1], pos[2]});
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The models whose bones were written this frame: each one's Update (vtable +0x18) once, after every write, as
// mafia-ragdoll updates its posed models (the bones' world matrices and the skinned mesh's from them). Whether the
// skin needs it as well as the bones' own flags was not measured: a first test that seemed to say so had a pose
// that left the arm where the animation had it.
std::vector<void *> g_dirty;
void Dirty(void *model) { if (model && std::find(g_dirty.begin(), g_dirty.end(), model) == g_dirty.end()) g_dirty.push_back(model); }
bool UpdateModel(void *model) {
    __try {
        using UpdateFn = void(__stdcall *)(void *);
        reinterpret_cast<UpdateFn>((*reinterpret_cast<void ***>(model))[0x18 / 4])(model);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void *ParentOf(void *f) {
    void *parent = nullptr;
    __try { parent = game::at<void *>(f, game::kFrameParent); } __except (EXCEPTION_EXECUTE_HANDLER) { parent = nullptr; }
    return parent;
}
// The people's and pedestrians' model frames with their owners, gathered once a frame (bone_set asks for each bone
// it writes: a ragdoll's twenty a frame), dropped when the frame's poses are flushed, a person goes and the mission
// closes. The pedestrians' elements are read once here: ModelOf on each would gather them all again for each.
struct Owned { void *model, *owner; };
std::vector<Owned> g_models;
bool g_modelsKnown;
void *ReadModel(void *owner, int at) {
    void *m = nullptr;
    __try { m = game::at<void *>(owner, at); } __except (EXCEPTION_EXECUTE_HANDLER) { m = nullptr; }
    return m;
}
const std::vector<Owned> &Models() {
    if (!g_modelsKnown) {
        g_models.clear();
        for (void *h : g_humans) if (void *m = ReadModel(h, game::kActorFrame)) g_models.push_back({m, h});
        for (void *e : PedestrianElements()) if (void *m = ReadModel(e, kPedModel)) g_models.push_back({m, e});
        g_modelsKnown = true;
    }
    return g_models;
}
const Owned *Known(void *f) {
    for (const Owned &o : Models()) if (o.model == f) return &o;
    return nullptr;
}
// The model a bone belongs to: a person's (or a pedestrian's) model frame among its parents.
void *ModelOfBone(void *frame) {
    void *f = frame;
    for (int i = 0; i < 16 && f; i++) {
        if (Known(f)) return f;
        f = ParentOf(f);
    }
    return nullptr;
}

// The pose each person was last drawn in by a mod or the bridge (the bones' world matrices once his model is updated
// after RENDER_BEGIN): what human_get_up hands back (between frames the game's own animation has the bones again).
struct LastPose { void *human; DWORD tick; float w[MML_BONE_COUNT][16]; };
std::vector<LastPose> g_lastPoses;

// the bridge's held poses
struct Held { void *human; void *frame; float rot[4]; float pos[3]; bool hasRot, hasPos; DWORD until; };
std::vector<Held> g_held;

bool Alive(void *h) { return std::find(g_humans.begin(), g_humans.end(), h) != g_humans.end(); }
bool Posable(void *h) { return Alive(h) || PedestrianKnown(h); }   // a person, or a traffic pedestrian's element

struct Info { bool alive; float health; uint32_t type; char model[64]; bool hasPos; float pos[3]; void *vehicle; };
bool ReadInfo(void *h, Info *o) {
    __try {
        void *model = game::at<void *>(h, game::kActorFrame);
        o->alive = game::at<uint8_t>(h, game::kActorAlive) != 0;
        o->health = game::at<float>(h, game::kHumanHealth);
        o->type = game::at<uint32_t>(h, game::kActorType);
        if (model) {
            const char *n = game::at<const char *>(model, game::kFrameName);
            if (n && Readable(n, 1)) strncpy_s(o->model, n, _TRUNCATE);
            const float *w = game::WorldMatrix(model);
            if (w) { o->pos[0] = w[12]; o->pos[1] = w[13]; o->pos[2] = w[14]; o->hasPos = true; }
        }
        o->vehicle = game::at<void *>(h, game::kHumanVehicle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

json HumanJson(void *h, void *player) {
    json j = {{"actor", Hex(h)}, {"player", h == player}};
    Info o{};
    if (!ReadInfo(h, &o)) { j["unreadable"] = true; return j; }
    j["alive"] = o.alive;
    j["health"] = o.health;
    j["type"] = o.type;
    if (o.model[0]) j["model"] = o.model;
    if (o.hasPos) j["pos"] = {o.pos[0], o.pos[1], o.pos[2]};
    if (o.vehicle) j["vehicle"] = Hex(o.vehicle);
    return j;
}

} // namespace

void HumansCreated(void *h) { if (h && !Alive(h)) { g_humans.push_back(h); g_modelsKnown = false; } }
void HumansGone(void *actor) {
    g_humans.erase(std::remove(g_humans.begin(), g_humans.end(), actor), g_humans.end());
    g_modelsKnown = false;
    g_held.erase(std::remove_if(g_held.begin(), g_held.end(), [&](const Held &x) { return x.human == actor; }), g_held.end());
    g_lastPoses.erase(std::remove_if(g_lastPoses.begin(), g_lastPoses.end(), [&](const LastPose &x) { return x.human == actor; }), g_lastPoses.end());
}
void HumansMissionClosed() { g_humans.clear(); g_held.clear(); g_dirty.clear(); g_lastPoses.clear(); g_modelsKnown = false; }

void *HumanPlayer() {
    if (!game::g_exe) return nullptr;
    void *p = nullptr;
    __try { p = game::Player(); } __except (EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
    return p;
}

int HumansList(void **out, int max) {
    int n = 0;
    for (void *h : g_humans) { if (out && n < max) out[n] = h; n++; }
    return n;
}

int HumanSkeleton(void *human, MmlBone *out, int max) {
    if (!game::g_exe || !Posable(human)) return 0;
    void *f[MML_BONE_COUNT];
    if (!FindBones(human, f)) return 0;
    for (int i = 0; i < MML_BONE_COUNT && i < max; i++) {
        MmlBone b{};
        b.name = kBoneNames[i];
        b.frame = f[i];
        if (f[i]) ReadBone(f[i], &b);
        if (out) out[i] = b;
    }
    return MML_BONE_COUNT;
}

int BoneSet(void *frame, const float *rot, const float *pos) {
    if (!frame || !game::g_exe) return 0;
    // a bone of a known model is a frame (its parents read under a guard); any other is asked of the memory first
    void *model = ModelOfBone(frame);
    if (!model && !Readable(frame, 0x130)) return 0;
    if (!WriteBone(frame, rot, pos)) return 0;
    if (model) Dirty(model);
    else UpdateModel(frame);   // (a frame not of a person's model: itself and its children)
    return 1;
}

bool HumanBones(void *human, void **frames) { return game::g_exe && Posable(human) && FindBones(human, frames); }
bool HumanPosable(void *human) { return game::g_exe && Posable(human); }
bool HumanLastPose(void *human, float world[][16], unsigned maxAgeMs) {
    for (const LastPose &p : g_lastPoses)
        if (p.human == human && GetTickCount() - p.tick <= maxAgeMs) { memcpy(world, p.w, sizeof p.w); return true; }
    return false;
}
bool HumanKnown(void *human) { return Alive(human); }

// (render.cpp) after RENDER_BEGIN's mods: every model whose bones were written, updated once.
void HumansFlushPoses() {
    for (void *m : g_dirty) {
        UpdateModel(m);
        const Owned *o = Known(m);
        void *h = o ? o->owner : nullptr;
        void *f[MML_BONE_COUNT];
        if (!h || !FindBones(h, f)) continue;
        LastPose p{h, GetTickCount(), {}};
        bool ok = true;
        for (int i = 0; i < MML_BONE_COUNT && ok; i++) {
            if (!f[i]) continue;
            MmlBone b{};
            ok = ReadBone(f[i], &b);
            memcpy(p.w[i], b.world, sizeof p.w[i]);
        }
        if (!ok) continue;
        g_lastPoses.erase(std::remove_if(g_lastPoses.begin(), g_lastPoses.end(), [&](const LastPose &x) { return x.human == h; }), g_lastPoses.end());
        g_lastPoses.push_back(p);
    }
    g_dirty.clear();
    g_modelsKnown = false;   // (the next frame's people and pedestrians gathered anew)
}

// (render.cpp) RENDER_BEGIN of the mission's world, before the mods': the poses the bridge holds.
void HumansBeforeRender() {
    AnimsBeforeRender();   // (anims.cpp: a get-up drawn from the body's pose into the game's animation)
    if (g_held.empty()) return;
    DWORD now = GetTickCount();
    g_held.erase(std::remove_if(g_held.begin(), g_held.end(), [&](const Held &x) { return static_cast<int>(x.until - now) <= 0; }), g_held.end());
    for (const Held &x : g_held)
        if (WriteBone(x.frame, x.hasRot ? x.rot : nullptr, x.hasPos ? x.pos : nullptr)) Dirty(ModelOf(x.human));
}

bool HumansMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "humans.list" && method != "human.skeleton" && method != "human.pose") return false;
    *ok = true;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    void *player = game::g_exe ? game::Player() : nullptr;
    if (method == "humans.list") {
        json out = json::array();
        for (void *h : g_humans) out.push_back(HumanJson(h, player));
        *reply = json({{"count", g_humans.size()}, {"humans", out}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    bool ped = p.contains("pedestrian");
    std::string a = ped ? p.value("pedestrian", "") : p.value("actor", "player");
    void *h = a == "player" && !ped ? player : reinterpret_cast<void *>(ParseAddress(a));
    if (!h || !(ped ? PedestrianKnown(h) : Alive(h))) {
        *ok = false;
        *reply = method + (ped ? ": no such pedestrian (pedestrians.list lists them)" : ": no such person (humans.list lists them; \"player\")");
        return true;
    }
    if (method == "human.skeleton") {
        MmlBone b[MML_BONE_COUNT];
        if (!HumanSkeleton(h, b, MML_BONE_COUNT)) { *ok = false; *reply = "human.skeleton: his model has no skeleton the loader knows (no thighs?)"; return true; }
        json bones = json::array();
        auto r4 = [](float v) { return std::round(v * 10000) / 10000; };
        for (const MmlBone &x : b) {
            if (!x.frame) { bones.push_back({{"name", x.name}, {"frame", nullptr}}); continue; }
            bones.push_back({{"name", x.name}, {"frame", Hex(x.frame)}, {"world_pos", {r4(x.world[12]), r4(x.world[13]), r4(x.world[14])}},
                             {"rot", {r4(x.rot[0]), r4(x.rot[1]), r4(x.rot[2]), r4(x.rot[3])}}, {"pos", {r4(x.pos[0]), r4(x.pos[1]), r4(x.pos[2])}},
                             {"x_axis", {r4(x.world[0]), r4(x.world[1]), r4(x.world[2])}}, {"y_axis", {r4(x.world[4]), r4(x.world[5]), r4(x.world[6])}},
                             {"z_axis", {r4(x.world[8]), r4(x.world[9]), r4(x.world[10])}}});
        }
        *reply = json({{"actor", Hex(h)}, {"bones", bones}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    // human.pose {actor, bones: {"r_arm": {"rot": [w,x,y,z], "pos": [x,y,z]}, ...}, ms}: held, drawn at each frame
    void *f[MML_BONE_COUNT];
    if (!FindBones(h, f)) { *ok = false; *reply = "human.pose: no skeleton"; return true; }
    DWORD until = GetTickCount() + static_cast<DWORD>(std::min(std::max(p.value("ms", 3000), 0), 600000));
    int held = 0;
    if (p.contains("bones") && p["bones"].is_object()) {
        for (auto it = p["bones"].begin(); it != p["bones"].end(); ++it) {
            int k = -1;
            for (int i = 0; i < MML_BONE_COUNT; i++) if (it.key() == kBoneNames[i]) k = i;
            if (k < 0 || !f[k]) { *ok = false; *reply = "human.pose: no bone \"" + it.key() + "\" (human.skeleton names them)"; return true; }
            Held x{h, f[k], {1, 0, 0, 0}, {0, 0, 0}, false, false, until};
            const json &v = it.value();
            if (v.contains("rot") && v["rot"].is_array() && v["rot"].size() == 4) { for (int i = 0; i < 4; i++) x.rot[i] = v["rot"][i].get<float>(); x.hasRot = true; }
            if (v.contains("pos") && v["pos"].is_array() && v["pos"].size() == 3) { for (int i = 0; i < 3; i++) x.pos[i] = v["pos"][i].get<float>(); x.hasPos = true; }
            g_held.erase(std::remove_if(g_held.begin(), g_held.end(), [&](const Held &o) { return o.frame == x.frame; }), g_held.end());
            g_held.push_back(x);
            held++;
        }
    }
    if (p.value("clear", false)) { g_held.erase(std::remove_if(g_held.begin(), g_held.end(), [&](const Held &o) { return o.human == h; }), g_held.end()); }
    *reply = json({{"actor", Hex(h)}, {"held", held}, {"ms", static_cast<int>(until - GetTickCount())}, {"poses_held", g_held.size()}}).dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

} // namespace host
