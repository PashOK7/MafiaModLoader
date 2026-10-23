// Questions to the world (mml.h world_ray, world_sweep, world_near, world_shapes; docs/MODDING.md "Questions to the
// world").
//
// The game's own collision tests answer: g_collision::_TestLineH (a ray against the static world and the moving
// objects, as the bullets and the AI ask it) and TestSphereB through its thunk (a sphere swept, as a person's and a
// car's moves ask it); both take a mask for each part (-1: that part not tested). An actor to ignore is given to
// them the game's way: its moving objects found in the dynamic grid by their owner (+0x44) and set as the next
// tests' exclusion list (DynSetNextCollOwnerMult, as the game sets its shooter's), what was there put back after.
// The shapes are read from the game's two grids as mafia-ragdoll reads them (game_collision.cpp; the layouts the
// same in both builds):
//   g_collision +0x284 the static grid: float minX, minZ, maxX, maxZ, cellX, cellZ; u32 nx, nz; ...; +0x2C cell*[nx*nz]
//     a cell: u32 count, prim **prims. A primitive (dwords): [0] its kind in the low byte, its surface in the top one;
//     1..7 a triangle: [1..3] its corners' pointers, [4..7] its plane (the normal into the solid, d); 0x80 a box:
//     [8..10] local min, [11..13] local max, [14..29] its world matrix; 0x81 an axis-aligned box: [2..4] min, [5..7]
//     max; 0x82 a sphere: [2..4] centre, [5] radius; 0x83 a box: [2..4] local min, [5..7] local max, [8..23] its
//     matrix; 0x84 a pole: [2] x, [3] z, [4] radius.
//   g_collision +0x28C the dynamic grid: float minX, minZ, cellX, cellZ; u32 nx, nz; +0x18 cell*[nx*nz]
//     a cell: u32 ix, iz, ?, obj **begin, **end, **cap (objects filed by their centre). An object (dwords): [0] its
//     kind (0x40 a box, 0x42, 0x44 spheres), [2] its class bits (0x100 a person, 0x200 a car), [4..6] its centre,
//     [7] its bounding radius, [9..11] local min, [12..14] local max, [15] -> its world matrix, [17] its owner.
//
// The water: the static faces of the water's surfaces under a point (WorldWater).
//
// The surfaces' names: the game's own table, tables\materials.def (MaterialName).
//
// Bridge methods (game thread): world.ray, world.sweep, world.ground, world.water, world.near, world.shapes,
// world.materials.
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

constexpr int kStaticGrid = 0x284, kDynamicGrid = 0x28C, kExclArray = 0x2B8, kExclCount = 0x2BC, kExclThird = 0x2B0;
constexpr int kObjOwner = 0x44, kObjClasses = 8;

using LineFn = void *(__thiscall *)(void *coll, const V3 *from, const V3 *delta, V3 *hit, V3 *normal, uint32_t moving, uint32_t stat);
using SweepFn = void *(__stdcall *)(const V3 *from, const V3 *delta, float radius, V3 *at, V3 *normal, uint32_t moving, uint32_t stat);
using ExcludeFn = void(__thiscall *)(void *coll, void *objects, int count, int third);

uint8_t *Coll() { return static_cast<uint8_t *>(game::Collision()); }
bool Known() { return game::g_exe && game::Exe().testLineAll && game::Exe().collExclude && game::Exe().sphereSweep; }

uintptr_t g_faultAt;
int Note(EXCEPTION_POINTERS *e) {
    g_faultAt = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (Note(GetExceptionInformation())) { return GetExceptionCode(); }
}

// The grids, or null (no mission: no collision loaded).
uint8_t *StaticGrid() { return game::at<uint8_t *>(Coll(), kStaticGrid); }
uint8_t *DynamicGrid() { return game::at<uint8_t *>(Coll(), kDynamicGrid); }

// Each moving object whose cell overlaps [lo, hi] (one cell further out: objects are filed by their centre).
template <class F> void EachMoving(const V3 &lo, const V3 &hi, F f) {
    uint8_t *grid = DynamicGrid();
    if (!grid) return;
    const float *hdr = reinterpret_cast<const float *>(grid);
    uint32_t nx = game::at<uint32_t>(grid, 0x10), nz = game::at<uint32_t>(grid, 0x14);
    uint32_t **cells = game::at<uint32_t **>(grid, 0x18);
    if (!cells || !nx || !nz || !(hdr[2] > 0) || !(hdr[3] > 0) || nx > 4096 || nz > 4096) return;
    int x0 = std::max(0, static_cast<int>(std::floor((lo.x - hdr[0]) / hdr[2])) - 1);
    int x1 = std::min(static_cast<int>(nx) - 1, static_cast<int>(std::floor((hi.x - hdr[0]) / hdr[2])) + 1);
    int z0 = std::max(0, static_cast<int>(std::floor((lo.z - hdr[1]) / hdr[3])) - 1);
    int z1 = std::min(static_cast<int>(nz) - 1, static_cast<int>(std::floor((hi.z - hdr[1]) / hdr[3])) + 1);
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
            uint32_t *cell = cells[nx * z + x];
            if (!cell) continue;
            uint32_t **b = reinterpret_cast<uint32_t **>(cell[3]), **e = reinterpret_cast<uint32_t **>(cell[4]);
            if (!b || e < b || e - b > 100000) continue;
            for (uint32_t **it = b; it < e; it++)
                if (*it) f(*it);
        }
}

// Each static primitive (once) whose cell overlaps [lo, hi].
template <class F> void EachStatic(const V3 &lo, const V3 &hi, F f) {
    uint8_t *grid = StaticGrid();
    if (!grid) return;
    const float *hdr = reinterpret_cast<const float *>(grid);
    uint32_t nx = game::at<uint32_t>(grid, 0x18), nz = game::at<uint32_t>(grid, 0x1C);
    uint32_t **cells = game::at<uint32_t **>(grid, 0x2C);
    if (!cells || !nx || !nz || !(hdr[4] > 0) || !(hdr[5] > 0) || nx > 4096 || nz > 4096) return;
    int x0 = std::max(0, static_cast<int>(std::floor((lo.x - hdr[0]) / hdr[4])));
    int x1 = std::min(static_cast<int>(nx) - 1, static_cast<int>(std::floor((hi.x - hdr[0]) / hdr[4])));
    int z0 = std::max(0, static_cast<int>(std::floor((lo.z - hdr[1]) / hdr[5])));
    int z1 = std::min(static_cast<int>(nz) - 1, static_cast<int>(std::floor((hi.z - hdr[1]) / hdr[5])));
    std::vector<const uint32_t *> seen;
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
            uint32_t *cell = cells[nx * z + x];
            if (!cell) continue;
            uint32_t n = cell[0];
            uint32_t **prims = reinterpret_cast<uint32_t **>(cell[1]);
            if (!prims || n > 200000) continue;
            for (uint32_t i = 0; i < n; i++) if (prims[i]) seen.push_back(prims[i]);
        }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    for (const uint32_t *p : seen) f(p);
}

bool IsMoving(uint32_t first) { return (first & 0xC0) == 0x40; }

// The test's masks from `what`: -1 leaves a part out.
uint32_t MovingMask(uint32_t what) { return what & MML_WORLD_MOVING ? (what & (MML_WORLD_NO_PEOPLE | MML_WORLD_NO_CARS)) : 0xFFFFFFFFu; }
uint32_t StaticMask(uint32_t what) { return what & MML_WORLD_STATIC ? 0u : 0xFFFFFFFFu; }

// The moving objects an actor owns, near it.
void OwnedObjects(void *actor, std::vector<void *> *out) {
    V3 c = game::ActorWorldPos(actor);
    EachMoving(c - V3{12, 12, 12}, c + V3{12, 12, 12}, [&](uint32_t *o) {
        if (game::at<void *>(o, kObjOwner) == actor && out->size() < 64 && std::find(out->begin(), out->end(), o) == out->end()) out->push_back(o);
    });
}

// A test with an actor's objects passed: the exclusion list set the game's way and put back after (each step
// guarded on its own, so a fault in the test still puts the list back).
struct Exclusion { void *array; uint32_t count, third; bool on; };
void SetExclusion(std::vector<void *> &objs, Exclusion *saved) {
    uint8_t *c = Coll();
    saved->array = game::at<void *>(c, kExclArray);
    saved->count = game::at<uint32_t>(c, kExclCount);
    saved->third = game::at<uint32_t>(c, kExclThird);
    reinterpret_cast<ExcludeFn>(game::Exe().collExclude)(c, objs.data(), static_cast<int>(objs.size()), 0);
    saved->on = true;
}
void PutExclusionBack(const Exclusion &saved) {
    uint8_t *c = Coll();
    reinterpret_cast<ExcludeFn>(game::Exe().collExclude)(c, saved.array, static_cast<int>(saved.count), static_cast<int>(saved.third));
    // (a null array makes the count 0: the count as it was)
    if (!saved.array) game::at<uint32_t>(c, kExclCount) = saved.count;
}
// test() run with `ignore`'s objects passed; a fault anywhere: its code, the list put back.
template <class F> DWORD Excluding(void *ignore, F test) {
    std::vector<void *> objs;
    Exclusion saved{};
    if (ignore) {
        auto own = [&]() { OwnedObjects(ignore, &objs); };
        if (DWORD ex = Guarded(own)) return ex;
        if (!objs.empty()) {
            auto set = [&]() { SetExclusion(objs, &saved); };
            if (DWORD ex = Guarded(set)) return ex;
        }
    }
    DWORD ex = Guarded(test);
    if (saved.on) {
        auto back = [&]() { PutExclusionBack(saved); };
        if (DWORD e2 = Guarded(back)) ex = ex ? ex : e2;
    }
    return ex;
}

void Fill(const uint32_t *prim, const V3 &from, const V3 &to, const V3 &at, const V3 &n, MmlHit *hit) {
    std::memset(hit, 0, sizeof *hit);
    hit->size = sizeof *hit;
    hit->pos[0] = at.x; hit->pos[1] = at.y; hit->pos[2] = at.z;
    hit->normal[0] = n.x; hit->normal[1] = n.y; hit->normal[2] = n.z;
    V3 d = to - from;
    float l2 = Dot(d, d);
    hit->fraction = l2 > 1e-12f ? std::min(1.0f, std::max(0.0f, Dot(at - from, d) / l2)) : 0.0f;
    hit->flags = prim[0];
    hit->prim = prim;
    if (IsMoving(prim[0])) {
        hit->actor = game::at<void *>(const_cast<uint32_t *>(prim), kObjOwner);
        hit->classes = prim[kObjClasses / 4];
    }
}

int Ray(const V3 &from, const V3 &to, uint32_t what, void *ignore, MmlHit *hit, std::string *why) {
    if (!Known()) { *why = "this build's collision tests are not known"; return 0; }
    if (!game::Mission() || !StaticGrid()) { *why = "no mission (no collision loaded)"; return 0; }
    int r = 0;
    auto f = [&]() {
        V3 delta = to - from, at{}, n{};
        void *p = reinterpret_cast<LineFn>(game::Exe().testLineAll)(Coll(), &from, &delta, &at, &n, MovingMask(what), StaticMask(what));
        if (p) { Fill(static_cast<const uint32_t *>(p), from, to, at, n, hit); r = 1; }
    };
    if (Excluding(ignore, f)) { *why = "the ray faulted (" + DescribeAddress(g_faultAt) + ")"; return 0; }
    return r;
}

int Sweep(const V3 &from, const V3 &to, float radius, uint32_t what, void *ignore, MmlHit *hit, std::string *why) {
    if (!Known()) { *why = "this build's collision tests are not known"; return 0; }
    if (!game::Mission() || !StaticGrid()) { *why = "no mission (no collision loaded)"; return 0; }
    if (!(radius > 0.001f) || radius > 50) { *why = "radius: 0.001 to 50 metres"; return 0; }
    int r = 0;
    auto f = [&]() {
        V3 delta = to - from, at{}, n{};
        void *p = reinterpret_cast<SweepFn>(game::Exe().sphereSweep)(&from, &delta, radius, &at, &n, MovingMask(what), StaticMask(what));
        if (p) {
            Fill(static_cast<const uint32_t *>(p), from, to, at, n, hit);
            float l = std::sqrt(Dot(n, n));
            if (l > 1e-6f) for (float &c : hit->normal) c /= l;
            r = 1;
        }
    };
    if (Excluding(ignore, f)) { *why = "the sweep faulted (" + DescribeAddress(g_faultAt) + ")"; return 0; }
    return r;
}

struct Near { void *actor; float d; };
int NearActors(const V3 &at, float radius, int type, std::vector<Near> *out) {
    if (!game::g_exe || !game::Mission()) return 0;
    auto f = [&]() {
        std::vector<void *> all;
        game::ListActors(&all);
        game::ListTrafficCars(&all);   // (actors too, kept out of the game's lists)
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        for (void *a : all) {
            if (type && game::at<int>(a, game::kActorType) != type) continue;
            if (!game::at<void *>(a, game::kActorFrame)) continue;
            V3 d = game::ActorWorldPos(a) - at;
            float l = std::sqrt(Dot(d, d));
            if (l <= radius) out->push_back({a, l});
        }
        std::sort(out->begin(), out->end(), [](const Near &a, const Near &b) { return a.d < b.d; });
    };
    if (Guarded(f)) out->clear();
    return static_cast<int>(out->size());
}

void SetAxes(MmlShape *s, const float *m) {
    for (int i = 0; i < 3; i++) {
        V3 r{m[i * 4], m[i * 4 + 1], m[i * 4 + 2]};
        float l = std::sqrt(Dot(r, r));
        if (l < 1e-9f) l = 1;
        s->axes[i * 3] = r.x / l; s->axes[i * 3 + 1] = r.y / l; s->axes[i * 3 + 2] = r.z / l;
    }
}
// A box from its world matrix (row vectors, scaled rows) and its local min and max.
void BoxShape(MmlShape *s, const float *m, const float *mn, const float *mx) {
    s->kind = MML_SHAPE_BOX;
    SetAxes(s, m);
    float lc[3], sc[3];
    for (int i = 0; i < 3; i++) {
        lc[i] = (mn[i] + mx[i]) * 0.5f;
        sc[i] = std::sqrt(m[i * 4] * m[i * 4] + m[i * 4 + 1] * m[i * 4 + 1] + m[i * 4 + 2] * m[i * 4 + 2]);
        s->half[i] = (mx[i] - mn[i]) * 0.5f * sc[i];
    }
    for (int k = 0; k < 3; k++) s->center[k] = m[12 + k] + m[k] * lc[0] + m[4 + k] * lc[1] + m[8 + k] * lc[2];
}

// A static primitive as a shape; false: a kind not known.
bool StaticShape(const uint32_t *pr, MmlShape *s) {
    const float *f = reinterpret_cast<const float *>(pr);
    std::memset(s, 0, sizeof *s);
    s->flags = pr[0];
    s->prim = pr;
    unsigned type = pr[0] & 0xFF;
    if (type >= 1 && type <= 7) {
        const float *v[3] = {reinterpret_cast<const float *>(pr[1]), reinterpret_cast<const float *>(pr[2]), reinterpret_cast<const float *>(pr[3])};
        if (!v[0] || !v[1] || !v[2]) return false;
        s->kind = MML_SHAPE_TRIANGLE;
        for (int k = 0; k < 3; k++) for (int i = 0; i < 3; i++) s->corner[k][i] = v[k][i];
        V3 n{-f[4], -f[5], -f[6]};   // (stored into the solid)
        float l = std::sqrt(Dot(n, n));
        if (l > 1e-9f) n = n * (1.0f / l);
        s->normal[0] = n.x; s->normal[1] = n.y; s->normal[2] = n.z;
        return true;
    }
    switch (type) {
    case 0x80: BoxShape(s, f + 14, f + 8, f + 11); return true;
    case 0x81: {
        s->kind = MML_SHAPE_BOX;
        for (int i = 0; i < 3; i++) { s->center[i] = (f[2 + i] + f[5 + i]) * 0.5f; s->half[i] = (f[5 + i] - f[2 + i]) * 0.5f; s->axes[i * 4] = 1; }
        return true;
    }
    case 0x82: s->kind = MML_SHAPE_SPHERE; s->center[0] = f[2]; s->center[1] = f[3]; s->center[2] = f[4]; s->half[0] = f[5]; return true;
    case 0x83: BoxShape(s, f + 8, f + 2, f + 5); return true;
    case 0x84: s->kind = MML_SHAPE_POLE; s->center[0] = f[2]; s->center[2] = f[3]; s->half[0] = f[4]; return true;
    }
    return false;
}

bool MovingShape(const uint32_t *o, MmlShape *s) {
    const float *f = reinterpret_cast<const float *>(o);
    std::memset(s, 0, sizeof *s);
    s->flags = o[0];
    s->classes = o[2];
    s->prim = o;
    s->actor = reinterpret_cast<void *>(o[17]);
    unsigned type = o[0] & 0xFF;
    if (type == 0x40) {
        const float *m = reinterpret_cast<const float *>(o[15]);
        if (!m) return false;
        BoxShape(s, m, f + 9, f + 12);
        return true;
    }
    if (type == 0x42 || type == 0x44) {
        s->kind = MML_SHAPE_SPHERE;
        s->center[0] = f[4]; s->center[1] = f[5]; s->center[2] = f[6]; s->half[0] = f[7];
        return true;
    }
    return false;
}

// The shape's box in the world's axes.
void Bounds(const MmlShape &s, V3 *lo, V3 *hi) {
    if (s.kind == MML_SHAPE_TRIANGLE) {
        *lo = *hi = V3{s.corner[0][0], s.corner[0][1], s.corner[0][2]};
        for (int k = 1; k < 3; k++) {
            lo->x = std::min(lo->x, s.corner[k][0]); lo->y = std::min(lo->y, s.corner[k][1]); lo->z = std::min(lo->z, s.corner[k][2]);
            hi->x = std::max(hi->x, s.corner[k][0]); hi->y = std::max(hi->y, s.corner[k][1]); hi->z = std::max(hi->z, s.corner[k][2]);
        }
        return;
    }
    V3 c{s.center[0], s.center[1], s.center[2]}, e{s.half[0], s.half[0], s.half[0]};
    if (s.kind == MML_SHAPE_BOX) {
        e = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            e.x += std::fabs(s.axes[i * 3]) * s.half[i];
            e.y += std::fabs(s.axes[i * 3 + 1]) * s.half[i];
            e.z += std::fabs(s.axes[i * 3 + 2]) * s.half[i];
        }
    } else if (s.kind == MML_SHAPE_POLE) {
        e.y = 1e9f;
    }
    *lo = c - e;
    *hi = c + e;
}
bool Overlaps(const MmlShape &s, const V3 &lo, const V3 &hi) {
    V3 a, b;
    Bounds(s, &a, &b);
    return !(a.x > hi.x || b.x < lo.x || a.y > hi.y || b.y < lo.y || a.z > hi.z || b.z < lo.z);
}

int Shapes(const V3 &lo, const V3 &hi, uint32_t what, MmlShape *out, int max) {
    if (!game::g_exe || !game::Mission() || !StaticGrid()) return 0;
    int n = 0;
    auto f = [&]() {
        MmlShape s;
        if (what & MML_WORLD_STATIC)
            EachStatic(lo, hi, [&](const uint32_t *p) {
                if ((p[0] & 0x20) || !StaticShape(p, &s) || !Overlaps(s, lo, hi)) return;
                if (n < max && out) out[n] = s;
                n++;
            });
        if (what & MML_WORLD_MOVING) {
            uint32_t mask = what & (MML_WORLD_NO_PEOPLE | MML_WORLD_NO_CARS);
            std::vector<const uint32_t *> seen;
            EachMoving(lo, hi, [&](uint32_t *o) {
                if ((o[2] & mask) || std::find(seen.begin(), seen.end(), o) != seen.end()) return;
                seen.push_back(o);
                if (!MovingShape(o, &s) || !Overlaps(s, lo, hi)) return;
                if (n < max && out) out[n] = s;
                n++;
            });
        }
    };
    if (Guarded(f)) return std::min(n, max);
    return n;
}

// ---- the bridge
std::string HexOf(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}
json Arr(const V3 &v) { return json::array({std::round(v.x * 1000) / 1000, std::round(v.y * 1000) / 1000, std::round(v.z * 1000) / 1000}); }
json Arr3(const float *v) { return Arr(V3{v[0], v[1], v[2]}); }
bool IsVec(const json &v) { return v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number(); }
V3 Vec(const json &v) { return V3{v[0].get<float>(), v[1].get<float>(), v[2].get<float>()}; }

std::string ActorName(void *a) {
    std::string out;
    auto f = [&]() {
        void *fr = game::at<void *>(a, game::kActorFrame);
        const char *n = fr ? game::at<const char *>(fr, game::kFrameName) : nullptr;
        if (n) out.assign(n, strnlen(n, 64));
    };
    if (Guarded(f)) out.clear();
    return out;
}
const char *KindName(uint32_t first) {
    unsigned t = first & 0xFF;
    if (t >= 1 && t <= 7) return "triangle";
    switch (t) {
    case 0x80: case 0x83: return "box";
    case 0x81: return "aabb";
    case 0x82: return "sphere";
    case 0x84: return "pole";
    case 0x40: return "moving box";
    case 0x42: case 0x44: return "moving sphere";
    }
    return "?";
}
json HitJson(const MmlHit &h, const V3 &from) {
    V3 p{h.pos[0], h.pos[1], h.pos[2]};
    V3 d = p - from;
    json j = {{"hit", true}, {"pos", Arr(p)}, {"normal", Arr3(h.normal)}, {"distance", std::round(std::sqrt(Dot(d, d)) * 1000) / 1000},
              {"fraction", std::round(h.fraction * 10000) / 10000}, {"kind", KindName(h.flags)}, {"flags", HexOf(reinterpret_cast<void *>(static_cast<uintptr_t>(h.flags)))},
              {"prim", HexOf(h.prim)}};
    if (h.actor) {
        j["actor"] = HexOf(h.actor);
        j["classes"] = HexOf(reinterpret_cast<void *>(static_cast<uintptr_t>(h.classes)));
        std::string n = ActorName(h.actor);
        if (!n.empty()) j["name"] = n;
    } else {
        j["surface"] = h.flags >> 24;
    }
    return j;
}

// `what` from the bridge: "all" (default), "static", "moving", and no_people / no_cars.
uint32_t What(const json &p) {
    std::string w = p.value("what", "all");
    uint32_t what = w == "static" ? MML_WORLD_STATIC : w == "moving" ? MML_WORLD_MOVING : MML_WORLD_ALL;
    if (p.value("no_people", false)) what |= MML_WORLD_NO_PEOPLE;
    if (p.value("no_cars", false)) what |= MML_WORLD_NO_CARS;
    return what;
}

// A point: [x, y, z], or an actor ("player", "car", "0x...") at its model's position plus `lift` metres up.
bool Point(const json &v, float lift, V3 *out, void **actor, std::string *err) {
    if (IsVec(v)) { *out = Vec(v); return true; }
    if (!v.is_string()) { *err = "[x, y, z] or an actor (\"player\", \"car\", \"0x...\")"; return false; }
    void *a = nullptr;
    if (!ParseActor(v.get<std::string>(), &a, err)) return false;
    if (actor) *actor = a;
    *out = game::ActorWorldPos(a) + V3{0, lift, 0};
    return true;
}

// The ray's or sweep's ends: from [x, y, z] | an actor (1.5 m up) | "camera" (the last frame's, through the screen's
// middle or `screen`), to [x, y, z] | dir (and `length`, default 100 m).
bool Ends(const json &p, V3 *from, V3 *to, void **ignore, std::string *err) {
    json f = p.contains("from") ? p["from"] : json("camera");
    float length = p.value("length", 100.0f);
    V3 dir{0, 0, 0};
    bool haveDir = false;
    if (f.is_string() && f.get<std::string>() == "camera") {
        float s[2] = {400, 300};
        if (p.contains("screen") && p["screen"].is_array() && p["screen"].size() == 2) { s[0] = p["screen"][0].get<float>(); s[1] = p["screen"][1].get<float>(); }
        float o[3], d[3];
        if (!RenderRay(s[0], s[1], o, d)) { *err = "from \"camera\": no frame of the world drawn yet"; return false; }
        *from = V3{o[0], o[1], o[2]};
        dir = V3{d[0], d[1], d[2]};
        haveDir = true;
        if (!p.contains("ignore")) *ignore = game::Player();   // (the player's own body in front of the camera)
    } else {
        void *a = nullptr;
        if (!Point(f, 1.5f, from, &a, err)) { *err = "from: " + *err; return false; }
        if (a && !p.contains("ignore")) *ignore = a;
    }
    if (p.contains("to")) {
        if (!Point(p["to"], 1.0f, to, nullptr, err)) { *err = "to: " + *err; return false; }
        return true;
    }
    if (p.contains("dir")) {
        if (!IsVec(p["dir"])) { *err = "dir: [x, y, z]"; return false; }
        dir = Vec(p["dir"]);
        float l = std::sqrt(Dot(dir, dir));
        if (!(l > 1e-6f)) { *err = "dir: not zero"; return false; }
        dir = dir * (1.0f / l);
        haveDir = true;
    }
    if (!haveDir) { *err = "to: [x, y, z] | an actor, or dir: [x, y, z] (with length)"; return false; }
    *to = *from + dir * length;
    return true;
}

} // namespace

// The water under a point: rays down the static world, from 2 m above it, each one going on below the face the last
// one met, until a face of a water surface (31 deep, 44 shallow; materials.def: what the game's walk and its cars
// test) or 200 m; the water's bed the next face under it within 100 m.
int WorldWater(const float at[3], MmlWater *out) {
    if (!out || out->size < offsetof(MmlWater, prim) + sizeof out->prim) return 0;
    V3 from{at[0], at[1] + 2.0f, at[2]};
    const float bottom = at[1] - 200.0f;
    bool covered = false;
    for (int i = 0; i < 32 && from.y > bottom; i++) {
        MmlHit h{};
        std::string why;
        if (!Ray(from, V3{from.x, bottom, from.z}, MML_WORLD_STATIC, nullptr, &h, &why)) return 0;
        int s = static_cast<int>(h.flags >> 24);
        if (s == 31 || s == 44) {
            MmlWater w{};
            w.size = out->size;
            w.height = h.pos[1];
            w.surface = s;
            w.covered = covered ? 1 : 0;
            w.prim = h.prim;
            w.depth = -1;
            MmlHit b{};
            V3 under{from.x, h.pos[1] - 0.01f, from.z};
            if (Ray(under, under - V3{0, 100, 0}, MML_WORLD_STATIC, nullptr, &b, &why)) w.depth = h.pos[1] - b.pos[1];
            std::memcpy(out, &w, std::min<size_t>(out->size, sizeof w));
            return 1;
        }
        covered = true;
        from.y = h.pos[1] - 0.01f;
    }
    return 0;
}

int WorldRay(const float from[3], const float to[3], uint32_t what, void *ignore, MmlHit *hit) {
    std::string why;
    MmlHit h{};
    int r = Ray(V3{from[0], from[1], from[2]}, V3{to[0], to[1], to[2]}, what, ignore, &h, &why);
    if (r) *hit = h;
    return r;
}
int WorldSweep(const float from[3], const float to[3], float radius, uint32_t what, void *ignore, MmlHit *hit) {
    std::string why;
    MmlHit h{};
    int r = Sweep(V3{from[0], from[1], from[2]}, V3{to[0], to[1], to[2]}, radius, what, ignore, &h, &why);
    if (r) *hit = h;
    return r;
}
int WorldNear(const float at[3], float radius, int type, void **out, int max) {
    std::vector<Near> list;
    int n = NearActors(V3{at[0], at[1], at[2]}, radius, type, &list);
    for (int i = 0; i < n && i < max && out; i++) out[i] = list[i].actor;
    return n;
}
int WorldShapes(const float lo[3], const float hi[3], uint32_t what, MmlShape *out, int max) {
    return Shapes(V3{lo[0], lo[1], lo[2]}, V3{hi[0], hi[1], hi[2]}, what, out, std::max(0, max));
}

// The surfaces' names: tables\materials.def as the game read it (game_build.h materials, materialCount; game.h
// kMaterialSize), a record's name its first 0x20 bytes (Czech, Windows-1250). Kept once read, for the block the
// game holds (it reads the file once, at its start); nullptr out of the table or an empty name.
namespace {
uint32_t g_matTable;
std::vector<std::string> g_matNames;
// The table's names are its makers' Czech in Windows-1250 in every build and language ("Chodník", "Dřevo 30%"; seen
// in the 1.2 lab, where the system's code page 1251 made them "Chodnнk"): read in that code page, not the system's.
std::string CzechUtf8(const char *text) {
    wchar_t w[64];
    int n = MultiByteToWideChar(1250, 0, text, -1, w, 64);
    if (n <= 0) return std::string();
    char u[192];
    int m = WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, nullptr, nullptr);
    return m > 0 ? std::string(u) : std::string();
}
} // namespace
const char *MaterialName(int id) {
    if (!game::g_exe || !game::Exe().materials || !game::Exe().materialCount || id < 0) return nullptr;
    uint32_t table = 0, count = 0;
    if (!MemRead(game::Exe().materials, &table, 4) || !MemRead(game::Exe().materialCount, &count, 4) || !table || !count || count > 256) return nullptr;
    if (table != g_matTable || g_matNames.size() != count) {
        g_matNames.clear();
        for (uint32_t i = 0; i < count; i++) {
            char name[game::kMaterialName + 1] = {};
            MemRead(table + i * game::kMaterialSize, name, game::kMaterialName);
            g_matNames.push_back(CzechUtf8(name));
        }
        g_matTable = table;
    }
    if (static_cast<uint32_t>(id) >= count || g_matNames[id].empty()) return nullptr;
    return g_matNames[id].c_str();
}

bool CollideMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "world.ray" && method != "world.sweep" && method != "world.ground" && method != "world.near" && method != "world.shapes" &&
        method != "world.water" && method != "world.materials")
        return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "world.ray" || method == "world.sweep") {
            V3 from{}, to{};
            void *ignore = nullptr;
            if (!Ends(p, &from, &to, &ignore, &err)) { *reply = method + ": " + err; return true; }
            if (p.contains("ignore")) {
                const json &ig = p["ignore"];
                if (ig.is_string() && !ig.get<std::string>().empty() && !ParseActor(ig.get<std::string>(), &ignore, &err)) { *reply = "ignore: " + err; return true; }
                if (ig.is_null() || (ig.is_string() && ig.get<std::string>().empty())) ignore = nullptr;
            }
            MmlHit h{};
            int r = method == "world.ray" ? Ray(from, to, What(p), ignore, &h, &err) : Sweep(from, to, p.value("radius", 0.3f), What(p), ignore, &h, &err);
            if (!r && !err.empty()) { *reply = method + ": " + err; return true; }
            json out = r ? HitJson(h, from) : json{{"hit", false}};
            out["from"] = Arr(from);
            out["to"] = Arr(to);
            if (ignore) out["ignored"] = HexOf(ignore);
            *reply = dump(out);
        } else if (method == "world.ground") {
            V3 at{};
            json a = p.contains("at") ? p["at"] : json("player");
            if (!Point(a, 0.5f, &at, nullptr, &err)) { *reply = "at: " + err; return true; }
            float depth = p.value("depth", 100.0f);
            MmlHit h{};
            if (!Ray(at, at - V3{0, depth, 0}, MML_WORLD_STATIC, nullptr, &h, &err)) {
                if (!err.empty()) { *reply = "world.ground: " + err; return true; }
                *reply = dump({{"hit", false}, {"at", Arr(at)}, {"depth", depth}});
            } else {
                json out = HitJson(h, at);
                out["height"] = std::round(h.pos[1] * 1000) / 1000;
                out["at"] = Arr(at);
                *reply = dump(out);
            }
        } else if (method == "world.water") {
            V3 at{};
            json a = p.contains("at") ? p["at"] : json("player");
            if (!Point(a, 0, &at, nullptr, &err)) { *reply = "at: " + err; return true; }
            if (!Known() || !game::Mission() || !StaticGrid()) { *reply = "world.water: no mission (no collision loaded)"; return true; }
            MmlWater w{};
            w.size = sizeof w;
            float f[3] = {at.x, at.y, at.z};
            if (!WorldWater(f, &w)) *reply = dump({{"water", false}, {"at", Arr(at)}});
            else
                *reply = dump({{"water", true}, {"at", Arr(at)}, {"height", std::round(w.height * 1000) / 1000}, {"surface", w.surface},
                               {"deep", w.surface == 31}, {"depth", std::round(w.depth * 1000) / 1000}, {"covered", w.covered != 0},
                               {"below", std::round((w.height - at.y) * 1000) / 1000}, {"prim", HexOf(w.prim)}});
        } else if (method == "world.near") {
            V3 at{};
            json a = p.contains("at") ? p["at"] : json("player");
            if (!Point(a, 0, &at, nullptr, &err)) { *reply = "at: " + err; return true; }
            std::vector<Near> list;
            NearActors(at, p.value("radius", 30.0f), p.value("type", 0), &list);
            json arr = json::array();
            size_t max = static_cast<size_t>(std::max(1, p.value("max", 50)));
            for (size_t i = 0; i < list.size() && i < max; i++) {
                json j = {{"actor", HexOf(list[i].actor)}, {"type", game::at<int>(list[i].actor, game::kActorType)},
                          {"distance", std::round(list[i].d * 100) / 100}, {"pos", Arr(game::ActorWorldPos(list[i].actor))}};
                std::string n = ActorName(list[i].actor);
                if (!n.empty()) j["name"] = n;
                arr.push_back(j);
            }
            *reply = dump({{"count", list.size()}, {"at", Arr(at)}, {"actors", arr}});
        } else if (method == "world.shapes") {
            V3 lo{}, hi{};
            if (p.contains("lo") && p.contains("hi") && IsVec(p["lo"]) && IsVec(p["hi"])) {
                lo = Vec(p["lo"]);
                hi = Vec(p["hi"]);
            } else {
                V3 at{};
                json a = p.contains("at") ? p["at"] : json("player");
                if (!Point(a, 0, &at, nullptr, &err)) { *reply = "at: " + err + " (or lo, hi)"; return true; }
                float r = p.value("radius", 3.0f);
                lo = at - V3{r, r, r};
                hi = at + V3{r, r, r};
            }
            int max = std::min(std::max(p.value("max", 100), 0), 5000);
            std::vector<MmlShape> buf(5000);
            int n = Shapes(lo, hi, What(p), buf.data(), 5000);
            json kinds = json::object(), list = json::array();
            const char *names[] = {"?", "triangle", "box", "sphere", "pole"};
            for (int i = 0; i < std::min(n, 5000); i++) {
                const MmlShape &s = buf[i];
                std::string k = std::string(s.actor ? "moving " : "") + names[s.kind < 5 ? s.kind : 0];
                kinds[k] = kinds.value(k, 0) + 1;
                if (static_cast<int>(list.size()) >= max) continue;
                json j = {{"kind", k}, {"flags", HexOf(reinterpret_cast<void *>(static_cast<uintptr_t>(s.flags)))}};
                if (s.kind == MML_SHAPE_TRIANGLE) j["corners"] = {Arr3(s.corner[0]), Arr3(s.corner[1]), Arr3(s.corner[2])}, j["normal"] = Arr3(s.normal);
                else j["center"] = Arr3(s.center);
                if (s.kind == MML_SHAPE_BOX) j["half"] = Arr3(s.half), j["axes"] = {Arr3(s.axes), Arr3(s.axes + 3), Arr3(s.axes + 6)};
                if (s.kind == MML_SHAPE_SPHERE || s.kind == MML_SHAPE_POLE) j["radius"] = std::round(s.half[0] * 1000) / 1000;
                if (s.actor) {
                    j["actor"] = HexOf(s.actor);
                    j["classes"] = HexOf(reinterpret_cast<void *>(static_cast<uintptr_t>(s.classes)));
                } else {
                    j["surface"] = s.flags >> 24;
                    if (const char *name = MaterialName(static_cast<int>(s.flags >> 24))) j["material"] = name;
                }
                list.push_back(j);
            }
            *reply = dump({{"count", n}, {"lo", Arr(lo)}, {"hi", Arr(hi)}, {"kinds", kinds}, {"shapes", list}});
        } else if (method == "world.materials") {
            json list = json::array();
            for (int id = 0; id < 256; id++)
                if (const char *name = MaterialName(id)) list.push_back({{"id", id}, {"name", name}});
            json out = {{"materials", list}};
            if (list.empty()) out["why"] = game::g_exe && game::Exe().materials ? "the game has not read tables\\materials.def yet" : "this build's table is not known";
            *reply = dump(out);
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
