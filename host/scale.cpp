// The size things are drawn at (mml.h actor_scale; docs/MODDING.md "The size things are drawn at").
//
// An LS3D frame carries a scale (+0x90, three floats) that UpdateMatrix (1.2 ls3d+1A6E0, 1.0 +1A4A0) multiplies into
// its local matrix's rows, and the world matrices below it follow (UpdateWMatrixProc: local times the parent's
// world). Written into an actor's model frame it holds through the game's own SetPos and SetRot, but the game reads
// those matrices back: a taxi scaled 2 in its frame drove as another car (seen 2026-10-23, the 1.2 lab), people's
// dynamic spheres sit at their frames' places, and the bullets test the models' own geometry. So the scale is the
// frame's drawing alone: before the world's Render (render.cpp, after the mods' RENDER_BEGIN, before the shadow map
// and the mods' views) each scaled model frame's local matrix is kept, its rows multiplied by the scale and its
// translation moved so the pivot stays where it was, its scale field set (the world scale +0x114 the drawing reads
// follows it) with the flags that make UpdateMatrix keep the rows (0x10 built, 0x4 scaled) and its world matrix marked
// stale (0x20, 0x100 its bounds, 0x40000000 changed: the children follow), and the frame's Update run; once the world
// is drawn, the kept matrix, scale and flags are put back the same way. The game's tick never sees a scaled matrix.
// The pivot by default is the bottom of the model's bounds (the frame's hierarchy box, +0xB0 min, +0xBC max, in its
// own space, valid while +0xAC bit 0x100 is set), under its middle: a car is scaled from the road, not from its axles.
//
// The pulse: scale * (1 + amount * shape(t)), t the beats since a shared start (MML_SCALE_BEAT_NOW, the bridge's
// `beat`) times bpm, shifted by the phase: every pulsing actor at one bpm beats together.
//
// Bridge methods (game thread): actor.scale {actor, scale, pivot, bpm, amount, shape, phase; clear; beat},
// actors.scaled.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kFrameLocal = 0x50, kFrameScale = 0x90, kFrameFlags = 0xAC, kFrameBoxMin = 0xB0, kFrameBoxMax = 0xBC;
constexpr uint32_t kBuilt = 0x10, kScaled = 0x4, kWorldValid = 0x20, kBoundValid = 0x100, kChanged = 0x40000000;
enum { kVtUpdate = 0x18 };
constexpr size_t kMaxScaled = 4096;

using UpdateFn = int(__stdcall *)(void *frame);
using FrameFn = void(__fastcall *)(void *frame, void *edx);

struct Entry {
    MmlMod *mod;            // null: the bridge's
    void *who;              // the actor, or a pedestrian's element
    bool pedestrian;
    MmlActorScale s;
};
std::vector<Entry> g_entries;

// What a frame held before this frame's drawing scaled it.
struct Kept { void *frame; float local[16]; float scale[3]; uint32_t flags; };
std::vector<Kept> g_kept;

LARGE_INTEGER g_beatStart, g_freq;
uint64_t g_framesScaled, g_scaledLast, g_faults;
std::string g_why = "nothing scaled yet";

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
void *Vt(void *obj, int off) { return (*reinterpret_cast<void ***>(obj))[off / 4]; }

std::string HexOf(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

double BeatSeconds() {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    if (!g_beatStart.QuadPart) QueryPerformanceCounter(&g_beatStart);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_beatStart.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

// 0..1, its top on the beat (the squash: -1..1, tall on the beat, wide between: a cosine, slow at both ends)
float Shape(int shape, double beats) {
    double f = beats - std::floor(beats);
    switch (shape) {
    case MML_PULSE_SQUASH: return static_cast<float>(std::cos(2 * 3.14159265358979 * f));
    case MML_PULSE_SINE: return static_cast<float>(0.5 + 0.5 * std::cos(2 * 3.14159265358979 * f));
    case MML_PULSE_BOUNCE: return static_cast<float>(std::fabs(std::sin(3.14159265358979 * f)));
    default: return static_cast<float>(std::exp(-5.0 * f));
    }
}

bool Plain(const MmlActorScale &s) { return s.scale[0] == 1 && s.scale[1] == 1 && s.scale[2] == 1 && (s.bpm <= 0 || s.amount == 0); }

// Its model frames: one, or a train's every car (C_railway: its own model the first, the others model frames of
// their own in the sector, "<name>_vagon<i>", game.h kRailwayVagons).
void ModelsOf(const Entry &e, const std::vector<void *> &peds, std::vector<void *> *out) {
    auto f = [&] {
        if (e.pedestrian) {
            if (std::find(peds.begin(), peds.end(), e.who) == peds.end()) return;
            MmlPedestrian p{};
            if (PedestrianRead(e.who, &p) && p.spawned && p.model) out->push_back(p.model);
            return;
        }
        if (void *frame = game::at<void *>(e.who, game::kActorFrame)) out->push_back(frame);
        if (game::at<int>(e.who, game::kActorType) != game::kActorTypeRailway) return;
        int n = game::at<int>(e.who, game::kRailwayVagonCount);
        void **cars = game::at<void **>(e.who, game::kRailwayVagons);
        for (int i = 0; cars && i < n && i < 64; i++) if (cars[i]) out->push_back(cars[i]);
    };
    Guarded(f);
}

// The frame's local matrix scaled about the pivot (its own space), as UpdateMatrix leaves a built one.
void ScaleFrame(void *frame, const float s[3], const Entry &e) {
    reinterpret_cast<FrameFn>(ModuleBase("LS3DF.dll") + game::Ls3d().frameUpdateMatrix)(frame, nullptr);   // (built)
    float *m = &game::at<float>(frame, kFrameLocal);
    float *sc = &game::at<float>(frame, kFrameScale);
    uint32_t &fl = game::at<uint32_t>(frame, kFrameFlags);
    Kept k{frame, {}, {sc[0], sc[1], sc[2]}, fl};
    memcpy(k.local, m, sizeof k.local);
    float p[3] = {0, 0, 0};
    if (e.s.pivot == MML_SCALE_PIVOT_POINT) {
        p[0] = e.s.point[0]; p[1] = e.s.point[1]; p[2] = e.s.point[2];
    } else if (e.s.pivot == MML_SCALE_PIVOT_BOTTOM && (fl & kBoundValid)) {
        const float *lo = &game::at<float>(frame, kFrameBoxMin), *hi = &game::at<float>(frame, kFrameBoxMax);
        if (lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2]) { p[0] = (lo[0] + hi[0]) / 2; p[1] = lo[1]; p[2] = (lo[2] + hi[2]) / 2; }
    }
    // (row vectors: a point x in the frame's space is x * M; the pivot's place kept: t += sum p_i (1 - s_i) row_i)
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[12 + j] += p[i] * (1 - s[i]) * m[i * 4 + j];
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i * 4 + j] *= s[i];
    for (int i = 0; i < 3; i++) sc[i] = k.scale[i] * s[i];
    fl = (fl & ~(kWorldValid | kBoundValid)) | kBuilt | kScaled | kChanged;
    g_kept.push_back(k);
    reinterpret_cast<UpdateFn>(Vt(frame, kVtUpdate))(frame);
}

void Restore(const Kept &k) {
    float *m = &game::at<float>(k.frame, kFrameLocal);
    float *sc = &game::at<float>(k.frame, kFrameScale);
    memcpy(m, k.local, sizeof k.local);
    sc[0] = k.scale[0]; sc[1] = k.scale[1]; sc[2] = k.scale[2];
    game::at<uint32_t>(k.frame, kFrameFlags) = (k.flags & ~(kWorldValid | kBoundValid)) | kChanged;
    reinterpret_cast<UpdateFn>(Vt(k.frame, kVtUpdate))(k.frame);
}

Entry *Find(void *who) {   // (one size an actor: the last mod's or the bridge's)
    for (Entry &e : g_entries) if (e.who == who) return &e;
    return nullptr;
}

json EntryJson(const Entry &e) {
    static const char *shapes[] = {"beat", "sine", "bounce", "squash"};
    static const char *pivots[] = {"bottom", "origin", "point"};
    json j = {{e.pedestrian ? "pedestrian" : "actor", HexOf(e.who)}, {"by", e.mod ? ModIdOf(e.mod) : "bridge"},
              {"scale", {e.s.scale[0], e.s.scale[1], e.s.scale[2]}}, {"pivot", pivots[std::clamp(e.s.pivot, 0, 2)]}};
    if (e.s.pivot == MML_SCALE_PIVOT_POINT) j["point"] = {e.s.point[0], e.s.point[1], e.s.point[2]};
    if (e.s.bpm > 0) j["pulse"] = {{"bpm", e.s.bpm}, {"amount", e.s.amount}, {"shape", shapes[std::clamp(e.s.shape, 0, 3)]}, {"phase", e.s.phase}};
    return j;
}

} // namespace

int ActorScale(MmlMod *mod, void *actor, const MmlActorScale *s, std::string *err) {
    if (!game::g_exe || !game::g_ls3d || !game::Ls3d().frameUpdateMatrix) { *err = "not a known build of the game"; return 0; }
    if (s && s->size < sizeof(MmlActorScale)) { *err = "MmlActorScale.size: sizeof(MmlActorScale)"; return 0; }
    if (s && (s->flags & MML_SCALE_BEAT_NOW)) QueryPerformanceCounter(&g_beatStart);
    if (!actor) {
        if (s && (s->flags & MML_SCALE_BEAT_NOW)) return 1;
        *err = "no actor";
        return 0;
    }
    std::vector<void *> peds = PedestrianElements();
    bool pedestrian = std::find(peds.begin(), peds.end(), actor) != peds.end();
    if (!pedestrian) {
        uint32_t vt = 0;
        if (!MemRead(reinterpret_cast<uintptr_t>(actor), &vt, 4) || !game::InExeSection(".rdata", vt)) {
            *err = HexOf(actor) + " is neither an actor nor a traffic pedestrian's element";
            return 0;
        }
        void *frame = nullptr;
        if (!MemRead(reinterpret_cast<uintptr_t>(actor) + game::kActorFrame, &frame, 4) || !frame) {
            *err = HexOf(actor) + " has no model frame";
            return 0;
        }
    }
    Entry *e = Find(actor);
    if (!s || Plain(*s)) {
        if (e) g_entries.erase(g_entries.begin() + (e - g_entries.data()));
        return 1;
    }
    for (int i = 0; i < 3; i++)
        if (!(s->scale[i] > 0) || s->scale[i] > 1000) { *err = "scale: each of x, y, z above 0 (and up to 1000)"; return 0; }
    if (s->pivot < MML_SCALE_PIVOT_BOTTOM || s->pivot > MML_SCALE_PIVOT_POINT) { *err = "pivot: MML_SCALE_PIVOT_*"; return 0; }
    if (s->shape < MML_PULSE_BEAT || s->shape > MML_PULSE_SQUASH) { *err = "shape: MML_PULSE_*"; return 0; }
    if (!(s->bpm >= 0 && s->bpm <= 2000) || !(std::fabs(s->amount) <= 100)) { *err = "bpm 0..2000, amount -100..100"; return 0; }
    if (!e) {
        if (g_entries.size() >= kMaxScaled) { *err = "4096 scaled at once already"; return 0; }
        g_entries.push_back({mod, actor, pedestrian, {}});
        e = &g_entries.back();
    }
    e->mod = mod;
    e->pedestrian = pedestrian;
    e->s = *s;
    e->s.size = sizeof(MmlActorScale);
    e->s.flags = 0;
    return 1;
}

void ScaleBeforeRender() {
    if (g_entries.empty() || !g_kept.empty()) return;
    std::vector<void *> peds;
    if (std::any_of(g_entries.begin(), g_entries.end(), [](const Entry &e) { return e.pedestrian; })) peds = PedestrianElements();
    double t = BeatSeconds();
    // (the frames first: one drawn inside another scaled one, the player in his car, takes its size from it)
    std::vector<std::pair<const Entry *, void *>> todo;
    std::vector<void *> models;
    for (const Entry &e : g_entries) {
        models.clear();
        ModelsOf(e, peds, &models);
        for (void *frame : models)
            if (std::none_of(todo.begin(), todo.end(), [&](const auto &x) { return x.second == frame; })) todo.push_back({&e, frame});
    }
    std::vector<void *> frames;
    for (const auto &x : todo) frames.push_back(x.second);
    std::sort(frames.begin(), frames.end());
    for (const auto &[ep, frame] : todo) {
        const Entry &e = *ep;
        bool inside = false;
        auto up = [&] {
            void *p = game::at<void *>(frame, game::kFrameParent);
            for (int i = 0; p && i < 32 && !inside; i++, p = game::at<void *>(p, game::kFrameParent))
                inside = std::binary_search(frames.begin(), frames.end(), p);
        };
        if (Guarded(up) || inside) continue;
        float s[3] = {e.s.scale[0], e.s.scale[1], e.s.scale[2]};
        if (e.s.bpm > 0 && e.s.shape == MML_PULSE_SQUASH) {
            // (tall and narrow on the beat, low and wide between; its length, z, kept)
            float w = e.s.amount * Shape(e.s.shape, t * e.s.bpm / 60.0 + e.s.phase);
            s[0] = std::max(0.001f, s[0] * (1 - w));
            s[1] = std::max(0.001f, s[1] * (1 + w));
        } else if (e.s.bpm > 0) {
            float f = 1 + e.s.amount * Shape(e.s.shape, t * e.s.bpm / 60.0 + e.s.phase);
            for (float &v : s) v = std::max(0.001f, v * f);
        }
        if (s[0] == 1 && s[1] == 1 && s[2] == 1) continue;
        auto f = [&] { ScaleFrame(frame, s, e); };
        if (DWORD code = Guarded(f)) {
            g_faults++;
            char b[96];
            snprintf(b, sizeof b, "scaling %s's frame faulted (0x%08lX)", HexOf(e.who).c_str(), code);
            g_why = b;
        }
    }
    g_scaledLast = g_kept.size();
    if (!g_kept.empty()) { g_framesScaled++; g_why = "scaled"; }
}

void ScaleAfterRender() {
    for (auto it = g_kept.rbegin(); it != g_kept.rend(); ++it) {
        auto f = [&] { Restore(*it); };
        if (Guarded(f)) g_faults++;
    }
    g_kept.clear();
}

void ScaleActorGone(void *actor) {
    g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [&](const Entry &e) { return e.who == actor; }), g_entries.end());
}
void ScaleModGone(MmlMod *mod) {
    g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [&](const Entry &e) { return e.mod == mod; }), g_entries.end());
}
void ScaleMissionClose() { g_entries.clear(); }

namespace {
bool Vec3Of(const json &j, float *out) {
    if (!j.is_array() || j.size() != 3) return false;
    for (int i = 0; i < 3; i++) {
        if (!j[i].is_number()) return false;
        out[i] = j[i].get<float>();
    }
    return true;
}
} // namespace

bool ScaleMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "actor.scale" && method != "actors.scaled") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "actors.scaled") {
            json list = json::array();
            for (const Entry &e : g_entries) list.push_back(EntryJson(e));
            *reply = dump({{"scaled", list}, {"count", g_entries.size()}, {"frames_drawn", g_framesScaled}, {"drawn_last_frame", g_scaledLast},
                           {"faults", g_faults}, {"beat_seconds", BeatSeconds()}, {"last", g_why}});
            *ok = true;
            return true;
        }
        std::string err;
        if (p.value("beat", false)) QueryPerformanceCounter(&g_beatStart);
        if (p.contains("clear") && p["clear"].is_string() && p["clear"] == "all") {
            size_t n = g_entries.size();
            g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [](const Entry &e) { return !e.mod; }), g_entries.end());
            *reply = dump({{"cleared", n - g_entries.size()}});
            *ok = true;
            return true;
        }
        if (!p.contains("actor")) {
            if (p.value("beat", false)) { *reply = dump({{"beat", "now"}}); *ok = true; return true; }
            *reply = "actor.scale: actor (\"player\", \"car\", an address: an actor or a traffic pedestrian's element), scale (a number or "
                     "[x, y, z]), pivot (\"bottom\", \"origin\" or [x, y, z]), bpm, amount, shape (\"beat\", \"sine\", \"bounce\"), phase; clear "
                     "(true, or \"all\"); beat (the pulses' beat starts now)";
            return true;
        }
        void *actor = nullptr;
        const json &a = p["actor"];
        if (a.is_string()) {
            std::string text = a.get<std::string>();
            if (text != "player" && text != "car") actor = reinterpret_cast<void *>(ParseAddress(text));
            else if (!ParseActor(text, &actor, &err)) { *reply = "actor.scale: " + err; return true; }
        } else if (a.is_number()) {
            actor = reinterpret_cast<void *>(static_cast<uintptr_t>(a.get<uint64_t>()));
        }
        if (!actor) { *reply = "actor.scale: actor: \"player\", \"car\" or an address"; return true; }
        MmlActorScale s{};
        s.size = sizeof s;
        s.scale[0] = s.scale[1] = s.scale[2] = 1;
        bool clear = p.value("clear", false);
        if (p.contains("scale")) {
            const json &v = p["scale"];
            if (v.is_number()) s.scale[0] = s.scale[1] = s.scale[2] = v.get<float>();
            else if (!Vec3Of(v, s.scale)) { *reply = "actor.scale: scale, a number or [x, y, z]"; return true; }
        }
        if (p.contains("pivot")) {
            const json &v = p["pivot"];
            if (v.is_string() && v == "bottom") s.pivot = MML_SCALE_PIVOT_BOTTOM;
            else if (v.is_string() && v == "origin") s.pivot = MML_SCALE_PIVOT_ORIGIN;
            else if (Vec3Of(v, s.point)) s.pivot = MML_SCALE_PIVOT_POINT;
            else { *reply = "actor.scale: pivot, \"bottom\" (default), \"origin\" or [x, y, z] in the model's space"; return true; }
        }
        s.bpm = p.value("bpm", 0.0f);
        s.amount = p.value("amount", s.bpm > 0 ? 0.3f : 0.0f);
        s.phase = p.value("phase", 0.0f);
        std::string shape = p.value("shape", "beat");
        if (shape == "beat") s.shape = MML_PULSE_BEAT;
        else if (shape == "sine") s.shape = MML_PULSE_SINE;
        else if (shape == "bounce") s.shape = MML_PULSE_BOUNCE;
        else if (shape == "squash") s.shape = MML_PULSE_SQUASH;
        else { *reply = "actor.scale: shape, \"beat\", \"sine\", \"bounce\" or \"squash\""; return true; }
        if (!ActorScale(nullptr, actor, clear ? nullptr : &s, &err)) { *reply = "actor.scale: " + err; return true; }
        Entry *e = Find(actor);
        json r = e ? EntryJson(*e) : json{{"actor", HexOf(actor)}, {"scale", {1, 1, 1}}, {"own_size", true}};
        r["held"] = g_entries.size();
        *reply = dump(r);
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
