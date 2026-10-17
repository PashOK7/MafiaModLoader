// debug_views, routes: the route each person and each car the game moves is following, the point it is sent to now and the
// way it goes, drawn in the world (both builds).
//
// People. The game walks a person along a route in ai_path::Tick (1.2 0x5026C0, 1.0 0x40C970), thiscall(ai_path
// *this, uint, C_human *owner, bool, bool), ret 0x10, called every tick for the route and its owner. Each tick it
// hands HumanMoveTo (fastcall: ECX the owner, EDX &path->target) the point at +0x74, and counts the waypoint reached
// when the owner is within sqrt(+0x90) of the point at +0x80; then it stages the next one. The route object is freed
// and made anew by the person's actions often (a few seconds apart, often at the old address), so the mod copies
// what it draws inside its hook, while the game holds the object, and never reads a route later.
//
// Cars. Every car the game drives (the traffic, the police, a chase, a mission's driver) has a C_driver, ticked by
// C_driver::Tick (1.2 0x43C4F0, 1.0 0x483360), thiscall(C_driver *this, int dt_ms), ret 4. Its car is at +0x10, its
// path a vector of 0x70-byte nodes at +0x164 (end +0x168), the node it drives to at +0x170. A node holds up to four
// lane points (+0x00, +0x0C, +0x18, +0x24: across the road), the lane taken (byte +0x44) and flags (word +0x60;
// 0x8000: an edge). FillStartValues (1.2 0x43F050, 1.0 0x485EC0) puts the point the car drives to at +0x6C: the
// cursor's node's lane point, the lane its own, or the node before's when both nodes are edges. The traffic's paths
// are a few nodes long and made anew as the car goes; a chase's runs across the city.
//
// The traffic's own cars (those C_traffic_car::AI drives, 1.2 0x4AFC40, 1.0 0x4FBA80) are left out unless `traffic` is
// on: they are told by the call their tick comes from (the one at +0xD5B in both builds). A traffic car a chase takes
// over (the police's) is ticked from the chase, and drawn.
//
// What is drawn, each part a setting of its own (`people`, `cars` choose whose):
// - `route`: a person's waypoints (+0x24 of each record) as crosses joined by lines (green; the one he walks to in
//   yellow) and each waypoint's gate, the passage A..B the route was built through (grey); a car's path through the
//   lane points it takes (blue; the node it drives to in white, the nodes behind it dimmer) and each node's lane
//   points across the road (grey);
// - `trail`: the way each went over the last `trail_s` seconds of the world's time (cyan), and where it is now (red);
// - `steer`: the point it is sent to now (magenta, with a line to it); for a person also the point where the game
//   counts the waypoint reached with its radius (orange circle) and the detour points around a blocker (blue).
// Only those within `range` metres of the camera are drawn. One the game has not ticked for a few seconds is
// dropped, as is one the game destroys; every one when the mission closes. Time here is the world's: the mission's
// ticks summed, so a pause stops it.
//
// The people's layout (read in both builds' code: Tick, GetNormalWaypoint 1.2 0x501C60 / 1.0 0x40BF30,
// isLastNormalWaypoint, GetNextSpecWaypoint 1.2 0x501EF0 / 1.0 0x40C1C0): the normal waypoints are a vector of
// 0x3C-byte records at +0x1C (1.2) / +0x04 (1.0), the detour points a vector of 0x18-byte records at +0x2C (1.2) /
// +0x14 (1.0); the rest is at the same offsets in both. A record: gate A +0x00, gate B +0x0C, the waypoint +0x24, a
// web node's index +0x34, its kind +0x38 (0: reached at the gate's midpoint; 1, 2: at the web node). C_driver's
// fields used here are at the same offsets in both builds.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "views.h"

#define SECTION "routes."   // its settings' keys, under its section of the config

namespace {
const MmlHost *H;
MmlMod *M;
bool g_on = true;
bool g_showPeople = true, g_showCars = true, g_showTraffic = false;
bool g_showRoute = true, g_showTrail = true, g_showSteer = true;
float g_range = 150.0f;
double g_trailS = 10.0;          // the way kept, in seconds of the world's time
double g_clock = 0;              // the world's time: the mission's ticks summed (seconds)

// ---- the game's code, per build
struct Layout {
    const char *build;
    uintptr_t pathTick;      // ai_path::Tick
    uintptr_t normal;        // ai_path: vector<ai_path_node>: begin, end at +4
    uintptr_t spec;          // ai_path: vector<ai_path_specnode>: begin, end at +4
    uintptr_t driverTick;    // C_driver::Tick
    uintptr_t trafficCall;   // C_traffic_car::AI's call of it (+0xD5B in both builds): the traffic's cars
};
constexpr Layout kLayouts[] = {
    {"1.2", 0x005026C0, 0x1C, 0x2C, 0x0043C4F0, 0x004B099B},
    {"1.0", 0x0040C970, 0x04, 0x14, 0x00483360, 0x004FC7DB},
};
uintptr_t g_trafficReturn = 0;   // where that call returns (0: not found in this Game.exe, every car is drawn)
const Layout *g_layout = nullptr;
// both builds' ai_path::Tick start so (sub esp,18h; push ebx/ebp/esi/edi; mov edi,[esp+30h]; mov bl,20h; mov esi,ecx;
// mov ebp,[edi+68h]), and C_driver::Tick so (sub esp,144h; push ebx/ebp; mov ebp,ecx; push esi/edi; mov eax,[ebp+10h];
// lea ...): a hook is taken only on its bytes
constexpr uint8_t kPathTickStart[] = {0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x30,
                                      0xB3, 0x20, 0x8B, 0xF1, 0x8B, 0x6F, 0x68};
constexpr uint8_t kDriverTickStart[] = {0x81, 0xEC, 0x44, 0x01, 0x00, 0x00, 0x53, 0x55,
                                        0x8B, 0xE9, 0x56, 0x57, 0x8B, 0x45, 0x10, 0x8D};
// ai_path, shared by both builds
constexpr uintptr_t kCursor = 0x58;       // int: the normal waypoint followed (-1: none)
constexpr uintptr_t kSpecCursor = 0x5C;   // int: the detour point followed (-1: none)
constexpr uintptr_t kTarget = 0x74;       // float[3]: the point HumanMoveTo walks him to
constexpr uintptr_t kArrive = 0x80;       // float[3]: the point the waypoint is reached at
constexpr uintptr_t kState = 0x8C;        // 0 none, 1 a waypoint, 2 a detour point, 3 invalidated, 4 abandoned
constexpr uintptr_t kArriveR2 = 0x90;     // float: the reach's radius, squared
constexpr uintptr_t kHeaderBytes = 0x94;
constexpr size_t kNodeBytes = 0x3C, kSpecBytes = 0x18;
constexpr uintptr_t kNodeA = 0x00, kNodeB = 0x0C, kNodePos = 0x24, kNodeWeb = 0x34, kNodeKind = 0x38;
constexpr size_t kMaxNodes = 1024, kMaxSpec = 64;   // more is not a route (memory that is not one)
// C_driver, shared by both builds
constexpr uintptr_t kDrvCar = 0x10;       // C_car*
constexpr uintptr_t kDrvTarget = 0x6C;    // float[3]: the point the car drives to
constexpr uintptr_t kDrvPath = 0x164;     // vector<node>: begin, end at +4
constexpr uintptr_t kDrvCursor = 0x170;   // uint: the node it drives to
constexpr uintptr_t kDrvHeaderBytes = 0x174;
constexpr size_t kCarNodeBytes = 0x70;
constexpr uintptr_t kCarNodeLane = 0x44, kCarNodeFlags = 0x60;
constexpr uint16_t kCarNodeEdge = 0x8000;
constexpr size_t kMaxCarNodes = 4096;
constexpr double kCarPathEveryS = 0.25;   // a car's path is copied again this often (its cursor and target each tick)
constexpr uintptr_t kActorFrame = 0x68;   // C_actor: its model frame

typedef char(__fastcall *PathTickFn)(void *self, void *edx, uint32_t a1, void *owner, uint32_t a3, uint32_t a4);
typedef char(__fastcall *DriverTickFn)(void *self, void *edx, int dtMs);
void *g_pathOrig = nullptr;      // the chains' originals (the loader keeps them right while hooks come and go)
void *g_driverOrig = nullptr;

constexpr size_t kTrailMax = 4000;     // places one keeps at most, whatever `trail_s` says
constexpr size_t kMaxWalkers = 32, kMaxDrivers = 64;
constexpr double kExpireS = 4.0;       // one not ticked for this long (of the world's time) is dropped

struct V { float x, y, z; };
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Len(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V At(const uint8_t *p, uintptr_t off) { V v; std::memcpy(&v, p + off, sizeof v); return v; }

// the way one went: its places a little apart, each with the time it was last there; and the route it follows now,
// told by its ends (a new route is often made at the old one's address), with where it stood when that began
struct Way {
    struct Place { V p; double t; };
    std::vector<Place> trail;
    V now{};
    bool nowOk = false;
    double routeStart = 0;
    V routeOrigin{}, routeFirst{}, routeLast{};
    bool routeOriginOk = false;

    void Moved(bool ok, V p, float step) {
        nowOk = ok;
        if (!ok) return;
        now = p;
        if (trail.empty() || Len(p - trail.back().p) > step) {
            trail.push_back({p, g_clock});
            if (trail.size() > kTrailMax) trail.erase(trail.begin());
        } else {
            trail.back().t = g_clock;   // still there: one standing keeps its place in the trail
        }
    }
    void Route(V first, V last) {   // (before Moved: the origin is the place at the last tick)
        if (Len(first - routeFirst) <= 0.01f && Len(last - routeLast) <= 0.01f) return;
        routeFirst = first;
        routeLast = last;
        routeStart = g_clock;
        routeOrigin = now;
        routeOriginOk = nowOk;
    }
    void Trim() {
        size_t old = 0;
        while (old < trail.size() && g_clock - trail[old].t > g_trailS) old++;
        if (old) trail.erase(trail.begin(), trail.begin() + static_cast<std::ptrdiff_t>(old));
    }
    // how far the way lies from the route in the ground plane (x, z): every place kept since the route began against
    // the route as gone (from where it stood when it began, then through `pts`); the mean and the largest distance
    void Offset(const std::vector<V> &pts, float &meanM, float &maxM, size_t &n) const {
        meanM = maxM = 0;
        n = 0;
        std::vector<V> line;
        if (routeOriginOk) line.push_back(routeOrigin);
        line.insert(line.end(), pts.begin(), pts.end());
        if (line.size() < 2) return;
        double sum = 0;
        for (const Place &place : trail) {
            if (place.t < routeStart) continue;
            const V p = place.p;
            float best = 1e9f;
            for (size_t k = 1; k < line.size(); k++) {
                const V a = line[k - 1], b = line[k];
                const float dx = b.x - a.x, dz = b.z - a.z;
                const float len2 = dx * dx + dz * dz;
                float t = len2 > 1e-6f ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / len2 : 0.0f;
                t = std::fmax(0.0f, std::fmin(1.0f, t));
                const float ex = p.x - (a.x + dx * t), ez = p.z - (a.z + dz * t);
                best = std::fmin(best, std::sqrt(ex * ex + ez * ez));
            }
            sum += best;
            maxM = std::fmax(maxM, best);
            n++;
        }
        if (n) meanM = static_cast<float>(sum / n);
    }
};

struct Node { V a, b, pos; uint32_t web = 0, kind = 0; };

// one person the game walks along a route, as the last tick left him
struct Walker {
    uintptr_t owner = 0, path = 0;
    unsigned long long ticks = 0;
    double seen = 0;             // the last time the game ticked him (g_clock)
    std::vector<Node> nodes;     // the route's waypoints
    std::vector<V> spec;         // the detour points
    int cursor = -1, specCursor = -1;
    uint32_t state = 0;
    V target{}, arrive{};
    float arriveR = 0;
    Way way;
};
std::vector<Walker> g_walkers;

struct CarNode {
    V lane[4];                   // the lane points across the road
    uint8_t laneByte = 0;        // the lane its own byte names
    uint8_t laneTaken = 0;       // the lane the car takes there (the node before's when both are edges)
    uint16_t flags = 0;
    V Pos() const { return lane[laneTaken & 3]; }
};

// one car the game drives, as the last tick of its C_driver left it
struct Driver {
    uintptr_t driver = 0, car = 0;
    bool traffic = false;        // ticked by the traffic (C_traffic_car::AI)
    unsigned long long ticks = 0;
    double seen = 0, pathRead = -1;
    uint32_t pathBegin = 0, pathEnd = 0;
    std::vector<CarNode> nodes;
    uint32_t cursor = 0;
    V target{};
    Way way;
};
std::vector<Driver> g_drivers;

V g_eye{};
bool g_eyeOk = false;

// a vector [begin, end) of `stride`-byte records in a copied header: its count (-1: not a vector one can read)
long long VecCount(const uint8_t *hdr, uintptr_t off, size_t stride, size_t max, uint32_t &begin, uint32_t &end) {
    std::memcpy(&begin, hdr + off, 4);
    std::memcpy(&end, hdr + off + 4, 4);
    if (!begin) return 0;                                    // empty (never allocated, or freed)
    if (end < begin || (end - begin) % stride) return -1;
    const size_t n = (end - begin) / stride;
    return n > max ? -1 : static_cast<long long>(n);
}

// an actor's place: its model frame's world translation
bool ActorPos(uintptr_t actor, V &out) {
    uint32_t frame = 0;
    float m[16];
    if (!H->mem_read(actor + kActorFrame, &frame, 4) || !frame) return false;
    if (!H->frame_world(M, reinterpret_cast<void *>(static_cast<uintptr_t>(frame)), m)) return false;
    out = {m[12], m[13], m[14]};
    return true;
}

template <class T, class Key>
T &Slot(std::vector<T> &list, Key key, size_t max) {
    for (T &x : list) if (key(x)) return x;
    if (list.size() >= max) {   // the one the game has not ticked the longest makes room
        size_t oldest = 0;
        for (size_t k = 1; k < list.size(); k++) if (list[k].seen < list[oldest].seen) oldest = k;
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    list.emplace_back();
    return list.back();
}

// ---- people: copied while the game holds the route (at the end of its tick: what the next tick starts from)
void SnapshotPath(uintptr_t path, uintptr_t owner) {
    uint8_t hdr[kHeaderBytes];
    if (!H->mem_read(path, hdr, sizeof hdr)) return;
    Walker &w = Slot(g_walkers, [&](const Walker &x) { return x.owner == owner; }, kMaxWalkers);
    w.owner = owner;
    w.path = path;
    w.seen = g_clock;
    w.ticks++;
    std::memcpy(&w.cursor, hdr + kCursor, 4);
    std::memcpy(&w.specCursor, hdr + kSpecCursor, 4);
    std::memcpy(&w.state, hdr + kState, 4);
    w.target = At(hdr, kTarget);
    w.arrive = At(hdr, kArrive);
    float r2 = 0;
    std::memcpy(&r2, hdr + kArriveR2, 4);
    w.arriveR = r2 > 0 && r2 < 1e6f ? std::sqrt(r2) : 0;

    uint32_t begin = 0, end = 0;
    w.nodes.clear();
    const long long n = VecCount(hdr, g_layout->normal, kNodeBytes, kMaxNodes, begin, end);
    if (n > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(n) * kNodeBytes);
        if (H->mem_read(begin, raw.data(), raw.size())) {
            w.nodes.resize(static_cast<size_t>(n));
            for (size_t i = 0; i < w.nodes.size(); i++) {
                const uint8_t *r = raw.data() + i * kNodeBytes;
                Node &d = w.nodes[i];
                d.a = At(r, kNodeA);
                d.b = At(r, kNodeB);
                d.pos = At(r, kNodePos);
                std::memcpy(&d.web, r + kNodeWeb, 4);
                std::memcpy(&d.kind, r + kNodeKind, 4);
            }
        }
    }
    if (!w.nodes.empty()) w.way.Route(w.nodes.front().pos, w.nodes.back().pos);
    w.spec.clear();
    const long long s = VecCount(hdr, g_layout->spec, kSpecBytes, kMaxSpec, begin, end);
    if (s > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(s) * kSpecBytes);
        if (H->mem_read(begin, raw.data(), raw.size()))
            for (size_t i = 0; i < static_cast<size_t>(s); i++) w.spec.push_back(At(raw.data() + i * kSpecBytes, 0));
    }
    V p;
    const bool ok = ActorPos(owner, p);
    w.way.Moved(ok, p, 0.2f);
}

// ---- cars: copied at the end of C_driver::Tick, the path again when it is another or a while has passed
void SnapshotDriver(uintptr_t driver, bool traffic) {
    uint8_t hdr[kDrvHeaderBytes];
    if (!H->mem_read(driver, hdr, sizeof hdr)) return;
    uint32_t car = 0, begin = 0, end = 0;
    std::memcpy(&car, hdr + kDrvCar, 4);
    if (traffic && !g_showTraffic) {   // the traffic's car, not wanted: not copied (one a chase took is again)
        for (size_t k = g_drivers.size(); k-- > 0;)
            if (g_drivers[k].car == car) g_drivers.erase(g_drivers.begin() + static_cast<std::ptrdiff_t>(k));
        return;
    }
    const long long n = VecCount(hdr, kDrvPath, kCarNodeBytes, kMaxCarNodes, begin, end);
    if (!car || n <= 0) return;   // no car or no path: the game's Tick did nothing either
    Driver &d = Slot(g_drivers, [&](const Driver &x) { return x.car == car; }, kMaxDrivers);
    d.driver = driver;
    d.car = car;
    d.traffic = traffic;
    d.seen = g_clock;
    d.ticks++;
    std::memcpy(&d.cursor, hdr + kDrvCursor, 4);
    d.target = At(hdr, kDrvTarget);
    if (begin != d.pathBegin || end != d.pathEnd || d.pathRead < 0 || g_clock - d.pathRead >= kCarPathEveryS) {
        std::vector<uint8_t> raw(static_cast<size_t>(n) * kCarNodeBytes);
        if (H->mem_read(begin, raw.data(), raw.size())) {
            d.nodes.resize(static_cast<size_t>(n));
            for (size_t i = 0; i < d.nodes.size(); i++) {
                const uint8_t *r = raw.data() + i * kCarNodeBytes;
                CarNode &c = d.nodes[i];
                for (int l = 0; l < 4; l++) c.lane[l] = At(r, 12u * l);
                c.laneByte = r[kCarNodeLane];
                std::memcpy(&c.flags, r + kCarNodeFlags, 2);
                const bool edges = i && (d.nodes[i - 1].flags & kCarNodeEdge) && (c.flags & kCarNodeEdge);
                c.laneTaken = edges ? d.nodes[i - 1].laneByte : c.laneByte;
            }
            d.pathBegin = begin;
            d.pathEnd = end;
            d.pathRead = g_clock;
        }
    }
    if (!d.nodes.empty()) d.way.Route(d.nodes.front().Pos(), d.nodes.back().Pos());
    V p;
    const bool ok = ActorPos(car, p);
    d.way.Moved(ok, p, 0.5f);
}

// the game's ai_path::Tick and C_driver::Tick: the chain's original with the same arguments, then the state copied
char __fastcall PathDetour(void *self, void *edx, uint32_t a1, void *owner, uint32_t a3, uint32_t a4) {
    const char r = reinterpret_cast<PathTickFn>(g_pathOrig)(self, edx, a1, owner, a3, a4);
    SnapshotPath(reinterpret_cast<uintptr_t>(self), reinterpret_cast<uintptr_t>(owner));
    return r;
}
// (the traffic's cars are told by the call they come from: this detour is the chain's first, entered by the thunk's
// jmp, so its return address is the game's call; behind another mod's detour it is that detour, and the car drawn)
char __fastcall DriverDetour(void *self, void *edx, int dtMs) {
    const bool traffic = g_trafficReturn && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_trafficReturn;
    const char r = reinterpret_cast<DriverTickFn>(g_driverOrig)(self, edx, dtMs);
    SnapshotDriver(reinterpret_cast<uintptr_t>(self), traffic);
    return r;
}

template <class T>
void Age(std::vector<T> &list) {
    for (size_t k = list.size(); k-- > 0;) {
        if (g_clock - list[k].seen > kExpireS) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        list[k].way.Trim();
    }
}

// the world's time goes on: the way older than `trail_s` is let go, one not ticked for long is dropped
void OnMissionTick(const MmlEvent *ev, void *) {
    g_clock += ev->dt;
    Age(g_walkers);
    Age(g_drivers);
}

void OnFrame(const MmlEvent *, void *) {
    float o[3], d[3];
    g_eyeOk = H->camera_ray(M, 400, 300, o, d) != 0;
    if (g_eyeOk) g_eye = {o[0], o[1], o[2]};
}

// ---- drawing (device lines, as the other world views)
struct Vertex { float x, y, z; uint32_t color; };   // D3DFVF_XYZ | D3DFVF_DIFFUSE
constexpr float kLift = 0.08f;                      // over the ground the routes lie on
void Seg(std::vector<Vertex> &v, V a, V b, uint32_t c) {
    v.push_back({a.x, a.y + kLift, a.z, c});
    v.push_back({b.x, b.y + kLift, b.z, c});
}
void Cross(std::vector<Vertex> &v, V p, float r, uint32_t c) {
    Seg(v, p - V{r, 0, 0}, p + V{r, 0, 0}, c);
    Seg(v, p - V{0, r, 0}, p + V{0, r, 0}, c);
    Seg(v, p - V{0, 0, r}, p + V{0, 0, r}, c);
}
void Circle(std::vector<Vertex> &v, V p, float r, uint32_t c) {
    constexpr int kSides = 24;
    for (int i = 0; i < kSides; i++) {
        const float a0 = 6.2831853f * i / kSides, a1 = 6.2831853f * (i + 1) / kSides;
        Seg(v, p + V{r * std::cos(a0), 0, r * std::sin(a0)}, p + V{r * std::cos(a1), 0, r * std::sin(a1)}, c);
    }
}

const uint32_t kGreen = 0xFF40FF60u, kGreenDim = 0xA040FF60u, kGate = 0x90C0C0C0u, kYellow = 0xFFFFE040u;
const uint32_t kCyan = 0xFF40E0FFu, kRed = 0xFFFF4040u, kMagenta = 0xFFFF40FFu, kOrange = 0xFFFF9020u;
const uint32_t kBlue = 0xFF4080FFu, kCarRoute = 0xFF50A0FFu, kCarRouteDim = 0x8050A0FFu, kWhite = 0xFFFFFFFFu;

void DrawWay(std::vector<Vertex> &v, const Way &w, float r) {
    for (size_t k = 1; k < w.trail.size(); k++) Seg(v, w.trail[k - 1].p, w.trail[k].p, kCyan);
    Cross(v, w.now, r, kRed);
}

void DrawWalker(std::vector<Vertex> &v, const Walker &w) {
    if (g_showRoute) {
        for (size_t k = 0; k < w.nodes.size(); k++) {
            const Node &n = w.nodes[k];
            const bool cur = static_cast<int>(k) == w.cursor;
            Cross(v, n.pos, cur ? 0.6f : 0.35f, cur ? kYellow : kGreen);
            Seg(v, n.a, n.b, kGate);
            if (k) Seg(v, w.nodes[k - 1].pos, n.pos, kGreenDim);
        }
    }
    if (g_showTrail) DrawWay(v, w.way, 0.4f);
    if (g_showSteer && (w.state == 1 || w.state == 2)) {
        Cross(v, w.target, 0.7f, kMagenta);
        Seg(v, w.way.now, w.target, kMagenta);
        if (w.arriveR > 0) Circle(v, w.arrive, w.arriveR, kOrange);
        for (size_t k = 0; k < w.spec.size(); k++) {
            Cross(v, w.spec[k], static_cast<int>(k) == w.specCursor ? 0.5f : 0.3f, kBlue);
            if (k) Seg(v, w.spec[k - 1], w.spec[k], kBlue);
        }
    }
}

void DrawDriver(std::vector<Vertex> &v, const Driver &d) {
    if (g_showRoute) {
        for (size_t k = 0; k < d.nodes.size(); k++) {
            const CarNode &n = d.nodes[k];
            const bool cur = k == d.cursor, behind = k < d.cursor;
            for (int l = 1; l < 4; l++)   // the lane points across the road (those that differ)
                if (Len(n.lane[l] - n.lane[l - 1]) > 0.05f) Seg(v, n.lane[l - 1], n.lane[l], kGate);
            Cross(v, n.Pos(), cur ? 1.0f : 0.6f, cur ? kWhite : behind ? kCarRouteDim : kCarRoute);
            if (k) Seg(v, d.nodes[k - 1].Pos(), n.Pos(), behind || k == d.cursor ? kCarRouteDim : kCarRoute);
        }
    }
    if (g_showTrail) DrawWay(v, d.way, 0.8f);
    if (g_showSteer) {
        Cross(v, d.target, 1.2f, kMagenta);
        Seg(v, d.way.now, d.target, kMagenta);
    }
}

void OnRenderWorld(const MmlEvent *ev, void *) {
    if (!g_on || !MML_EVENT_HAS(ev, world) || !ev->world) return;
    auto *d = static_cast<IDirect3DDevice9 *>(H->render_device(9));
    if (!d) return;
    std::vector<Vertex> v;
    auto inRange = [](const Way &w) { return w.nowOk && (!g_eyeOk || Len(w.now - g_eye) <= g_range); };
    if (g_showPeople)
        for (const Walker &w : g_walkers) if (inRange(w.way)) DrawWalker(v, w);
    if (g_showCars)
        for (const Driver &c : g_drivers) if ((!c.traffic || g_showTraffic) && inRange(c.way)) DrawDriver(v, c);
    if (v.empty()) return;
    D3DMATRIX world = {};
    world._11 = world._22 = world._33 = world._44 = 1;
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->SetTexture(0, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->DrawPrimitiveUP(D3DPT_LINELIST, static_cast<UINT>(v.size() / 2), v.data(), sizeof(Vertex));
}

void ReadSettings() {
    g_on = H->config_number(M, SECTION "show", 1) != 0;
    g_showPeople = H->config_number(M, SECTION "people", 1) != 0;
    g_showCars = H->config_number(M, SECTION "cars", 1) != 0;
    g_showTraffic = H->config_number(M, SECTION "traffic", 0) != 0;
    g_showRoute = H->config_number(M, SECTION "route", 1) != 0;
    g_showTrail = H->config_number(M, SECTION "trail", 1) != 0;
    g_showSteer = H->config_number(M, SECTION "steer", 1) != 0;
    g_range = static_cast<float>(H->config_number(M, SECTION "range", 150));
    g_trailS = std::fmax(0.5, H->config_number(M, SECTION "trail_s", 10));
    H->hotkey(M, H->config_string(M, SECTION "hotkey", "F6"), [](void *) { g_on = !g_on; H->config_set(M, SECTION "show", g_on ? "true" : "false"); }, nullptr);
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_CLOSE) {   // its people and cars go with it
        g_walkers.clear();
        g_drivers.clear();
    }
    if (ev->type == MML_EVENT_ACTOR_DESTROYED) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(ev->actor);
        for (size_t k = g_walkers.size(); k-- > 0;)
            if (g_walkers[k].owner == a) g_walkers.erase(g_walkers.begin() + static_cast<std::ptrdiff_t>(k));
        for (size_t k = g_drivers.size(); k-- > 0;)
            if (g_drivers[k].car == a) g_drivers.erase(g_drivers.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

void Vec(std::string &out, V v) {
    char b[96];
    snprintf(b, sizeof b, "[%.3f,%.3f,%.3f]", v.x, v.y, v.z);
    out += b;
}

void WayJson(std::string &out, const Way &w, const std::vector<V> &route) {
    float meanM = 0, maxM = 0;
    size_t samples = 0;
    w.Offset(route, meanM, maxM, samples);
    const double span = w.trail.empty() ? 0 : g_clock - w.trail.front().t;
    char b[256];
    snprintf(b, sizeof b, "\"trail\":{\"places\":%zu,\"s\":%.2f,\"keep_s\":%.2f},"
             "\"way_vs_route\":{\"samples\":%zu,\"mean_m\":%.3f,\"max_m\":%.3f},\"now\":",
             w.trail.size(), span, g_trailS, samples, meanM, maxM);
    out += b;
    Vec(out, w.now);
}

// debug_views.routes.state: every person and car the game ticked, as data
int State(const char *, MmlReply *reply, void *) {
    std::string out = "{\"walkers\":[";
    char b[512];
    for (size_t k = 0; k < g_walkers.size(); k++) {
        const Walker &w = g_walkers[k];
        snprintf(b, sizeof b,
                 "%s{\"owner\":\"0x%08llX\",\"path\":\"0x%08llX\",\"ticks\":%llu,\"s_ago\":%.3f,\"state\":%u,\"cursor\":%d,"
                 "\"spec_cursor\":%d,\"arrive_radius\":%.3f,",
                 k ? "," : "", static_cast<unsigned long long>(w.owner), static_cast<unsigned long long>(w.path), w.ticks,
                 g_clock - w.seen, w.state, w.cursor, w.specCursor, w.arriveR);
        out += b;
        std::vector<V> route;
        for (const Node &n : w.nodes) route.push_back(n.pos);
        WayJson(out, w.way, route);
        out += ",\"target\":";
        Vec(out, w.target);
        out += ",\"arrive\":";
        Vec(out, w.arrive);
        out += ",\"waypoints\":[";
        for (size_t i = 0; i < w.nodes.size(); i++) {
            const Node &n = w.nodes[i];
            out += i ? ",{\"pos\":" : "{\"pos\":";
            Vec(out, n.pos);
            out += ",\"gate\":[";
            Vec(out, n.a);
            out += ",";
            Vec(out, n.b);
            snprintf(b, sizeof b, "],\"kind\":%u,\"web_node\":%u}", n.kind, n.web);
            out += b;
        }
        out += "],\"detour\":[";
        for (size_t i = 0; i < w.spec.size(); i++) {
            if (i) out += ",";
            Vec(out, w.spec[i]);
        }
        out += "]}";
    }
    out += "],\"drivers\":[";
    for (size_t k = 0; k < g_drivers.size(); k++) {
        const Driver &d = g_drivers[k];
        snprintf(b, sizeof b, "%s{\"car\":\"0x%08llX\",\"driver\":\"0x%08llX\",\"traffic\":%s,\"ticks\":%llu,\"s_ago\":%.3f,\"cursor\":%u,\"nodes\":%zu,",
                 k ? "," : "", static_cast<unsigned long long>(d.car), static_cast<unsigned long long>(d.driver),
                 d.traffic ? "true" : "false", d.ticks, g_clock - d.seen, d.cursor, d.nodes.size());
        out += b;
        std::vector<V> route;
        for (const CarNode &n : d.nodes) route.push_back(n.Pos());
        WayJson(out, d.way, route);
        out += ",\"target\":";
        Vec(out, d.target);
        out += ",\"path\":[";
        for (size_t i = 0; i < d.nodes.size(); i++) {
            const CarNode &n = d.nodes[i];
            out += i ? ",{\"pos\":" : "{\"pos\":";
            Vec(out, n.Pos());
            out += ",\"lanes\":[";
            for (int l = 0; l < 4; l++) {
                if (l) out += ",";
                Vec(out, n.lane[l]);
            }
            snprintf(b, sizeof b, "],\"lane\":%u,\"lane_byte\":%u,\"flags\":\"0x%04X\"}", n.laneTaken, n.laneByte, n.flags);
            out += b;
        }
        out += "]}";
    }
    out += "]}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}

// a hook on a game function whose first bytes are as expected
bool HookAt(uintptr_t at, const uint8_t *start, size_t n, void *detour, void **orig, const char *what) {
    uint8_t got[32];
    if (n > sizeof got || !H->mem_read(at, got, n) || std::memcmp(got, start, n) != 0) {
        H->log(M, MML_LOG_WARN, "routes: %s is not at 0x%08X in this Game.exe", what, static_cast<unsigned>(at));
        return false;
    }
    if (!H->hook(M, at, detour, orig)) {
        H->log(M, MML_LOG_WARN, "routes: %s could not be hooked", what);
        return false;
    }
    return true;
}
} // namespace

int RoutesLoad(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, hook) || !MML_HAS(host, frame_world) || !MML_HAS(host, camera_ray)) return 1;   // a loader older than this mod
    for (const Layout &l : kLayouts)
        if (std::strcmp(host->game_build(), l.build) == 0) g_layout = &l;
    if (!g_layout) {
        host->log(mod, MML_LOG_ERROR, "routes: game %s has no layout here", host->game_build());
        return 1;
    }
    const bool people = HookAt(g_layout->pathTick, kPathTickStart, sizeof kPathTickStart, reinterpret_cast<void *>(&PathDetour), &g_pathOrig, "ai_path::Tick");
    const bool cars = HookAt(g_layout->driverTick, kDriverTickStart, sizeof kDriverTickStart, reinterpret_cast<void *>(&DriverDetour), &g_driverOrig, "C_driver::Tick");
    if (!people && !cars) return 1;
    // the traffic's call of C_driver::Tick: an E8 call there, to it (else every car is drawn as one of the missions')
    uint8_t call[5];
    int32_t rel = 0;
    if (cars && host->mem_read(g_layout->trafficCall, call, sizeof call) && call[0] == 0xE8 &&
        (std::memcpy(&rel, call + 1, 4), g_layout->trafficCall + 5 + rel == g_layout->driverTick))
        g_trafficReturn = g_layout->trafficCall + 5;
    else if (cars)
        host->log(mod, MML_LOG_WARN, "routes: the traffic's call of C_driver::Tick is not at 0x%08X: its cars are not told apart", static_cast<unsigned>(g_layout->trafficCall));
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnMissionTick, nullptr);
    host->subscribe(mod, MML_EVENT_RENDER_WORLD, OnRenderWorld, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_ACTOR_DESTROYED, OnEvent, nullptr);
    host->command(mod, "routes.state", "every person and car the game moves along a route: its waypoints or path, the point it is sent to, its way against the route", State, nullptr);
    host->log(mod, MML_LOG_INFO, "routes loaded (game %s; people %s, cars %s)", host->game_build(), people ? "on" : "off", cars ? "on" : "off");
    return 0;
}

void RoutesUnload() {}
