// physx_crates's 64-bit half (sdk/include/mml/side.h; the 32-bit half: physx_crates.cpp; the records: physx_crates.h;
// docs/MODDING.md "64-bit halves"). PhysX 5 (pinned by tools/deps.py, built by the x64 preset: CMake target PhysX5)
// steps a pile of crates on a thread of this half's own.
//
// Each FRAME event wakes the worker (the sidecar's mod thread only passes the word on). The worker reads the channel
// "frame" (the frame the 32-bit half began, its time, the moving objects around the pile); when it names a pile it has
// not built yet, it reads "setup" and builds a new scene from it: the static world around the pile as the game's own
// collision keeps it (its triangles cooked into one PhysX triangle mesh, its boxes, spheres and poles as shapes), the
// crates as dynamic boxes with the places and velocities given. Then the frame's step: the moving objects (people,
// cars) are kinematic bodies moved to their new places over the step's substeps (at most 1/90 s each), so they push
// the crates and nothing pushes them; simulate, fetchResults. The bodies' poses go back in the channel "poses", named
// by the frame: the 32-bit half places its props from them before the frame's scene is drawn.
//
// PhysX's error callback and its two worker threads (PxDefaultCpuDispatcher) never call the host: the errors are kept
// and logged on the mod thread. The worker's step is guarded: a fault in it (or in PhysX) stops the stepping and is
// reported (`side`), the sidecar goes on.
//   call physx_crates.side  -> {"steps", "step_ms": {"last", "mean", "max"}, "bodies", "awake", "movers",
//                               "triangles", "statics", "builds", "build_ms", "error"}: what this half has done
//   call physx_crates.fault -> a fault on purpose on the sidecar's mod thread: the half is unloaded, the rest goes
//                              on (MOD_FAULTED with "half": "x64"; mod.reload brings it back)
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <PxPhysicsAPI.h>
#include "mml/side.h"
#include "physx_crates.h"

using namespace physx;

namespace {
const MmlSideHost *H;
MmlSideMod *M;
MmlSideChannel *g_setupCh, *g_frameCh, *g_posesCh;

std::mutex g_errMx;
std::string g_error;       // the last error (PhysX's or this half's), for `side`
std::string g_toLog;       // errors not logged yet (the mod thread logs them)
void Error(const std::string &e) {
    std::lock_guard<std::mutex> lk(g_errMx);
    g_error = e;
    if (g_toLog.size() < 4096) g_toLog += e + "\n";
}

class ErrorCallback : public PxErrorCallback {
public:
    void reportError(PxErrorCode::Enum code, const char *message, const char *file, int line) override {
        if (code == PxErrorCode::eDEBUG_INFO) return;
        char b[512];
        snprintf(b, sizeof b, "PhysX (%d): %s (%s:%d)", static_cast<int>(code), message, file ? file : "", line);
        Error(b);
    }
};

PxDefaultAllocator g_alloc;
ErrorCallback g_errors;
PxFoundation *g_foundation;
PxPhysics *g_physics;
PxDefaultCpuDispatcher *g_dispatcher;
PxMaterial *g_material;

// The scene of one pile (a new setup: a new scene).
struct Mover {
    PxRigidDynamic *body;
    PxTransform last;
    uint64_t seen;     // the frame it was last in
};
struct World {
    PxScene *scene = nullptr;
    std::vector<PxRigidDynamic *> bodies;
    std::vector<uint8_t> lost;
    std::unordered_map<uint64_t, Mover> movers;
    uint32_t serial = 0, triangles = 0, statics = 0;
    float lowest = -1e30f;
};
World g_world;
std::unique_ptr<crates::Setup> g_setup = std::make_unique<crates::Setup>();
crates::Frame g_frame;
crates::Poses g_poses;

std::thread g_worker;
HANDLE g_wake;
std::atomic<bool> g_stop, g_broken;
std::atomic<uint64_t> g_steps, g_builds;
std::atomic<double> g_lastMs, g_sumMs, g_maxMs, g_buildMs;
double g_freqMs;

template <class T> void Release(T *&p) { if (p) { p->release(); p = nullptr; } }
double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_freqMs;
}

// Contacts as the default shader makes them, and swept (CCD) ones too: a crate struck by a car at 20 m/s does not pass
// through a wall's triangle in one substep.
PxFilterFlags FilterShader(PxFilterObjectAttributes a0, PxFilterData, PxFilterObjectAttributes a1, PxFilterData,
                           PxPairFlags &pairFlags, const void *, PxU32) {
    if (PxFilterObjectIsTrigger(a0) || PxFilterObjectIsTrigger(a1)) {
        pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
        return PxFilterFlag::eDEFAULT;
    }
    pairFlags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT;
    return PxFilterFlag::eDEFAULT;
}

// A box's rotation from the game's axes (rows: its x, y, z in the world). A box is the same with an axis reversed, so
// a left-handed set is made right-handed by its z; a set that is not quite orthonormal is made so (Gram-Schmidt).
PxQuat AxesQuat(const float *a) {
    PxVec3 x(a[0], a[1], a[2]), y(a[3], a[4], a[5]), z(a[6], a[7], a[8]);
    x.normalizeSafe();
    y = (y - x * x.dot(y)).getNormalized();
    if (y.isZero()) return PxQuat(PxIdentity);
    PxVec3 zz = x.cross(y);
    if (zz.dot(z) < 0) zz = -zz, y = -y;   // (keeps x; the pair y, z reversed together is still the box)
    z = x.cross(y);
    PxQuat q(PxMat33(x, y, z));
    q.normalize();
    return q;
}
PxTransform MoverPose(const crates::Mover &m) {
    PxVec3 c(m.center[0], m.center[1], m.center[2]);
    return m.kind == crates::kBox ? PxTransform(c, AxesQuat(m.axes)) : PxTransform(c);
}

void Clear() {
    Release(g_world.scene);   // (the scene's actors go with it)
    g_world = World{};
}

// The scene for a new pile: the statics, the crates.
bool Build(const crates::Setup &s) {
    double t0 = Now();
    Clear();
    PxSceneDesc sd(g_physics->getTolerancesScale());
    sd.gravity = PxVec3(0, -9.81f, 0);
    sd.cpuDispatcher = g_dispatcher;
    sd.filterShader = FilterShader;
    sd.flags |= PxSceneFlag::eENABLE_CCD;
    g_world.scene = g_physics->createScene(sd);
    if (!g_world.scene) { Error("createScene failed"); return false; }
    g_world.serial = s.serial;
    g_world.lowest = s.lo[1] - 20;

    // the static world: one actor, its triangles one mesh, its other primitives shapes of their own
    PxRigidStatic *ground = g_physics->createRigidStatic(PxTransform(PxIdentity));
    std::vector<PxVec3> verts;
    std::vector<PxU32> index;
    uint32_t n = s.statics < crates::kMaxStatics ? s.statics : crates::kMaxStatics;
    float tall = s.hi[1] - s.lo[1] + 40;   // a pole's height: through the whole region
    for (uint32_t i = 0; i < n; i++) {
        const crates::Static &st = s.shape[i];
        const float *v = st.v;
        switch (st.kind) {
        case crates::kTriangle: {
            PxVec3 a(v[0], v[1], v[2]), b(v[3], v[4], v[5]), c(v[6], v[7], v[8]), nrm(v[9], v[10], v[11]);
            if ((b - a).cross(c - a).dot(nrm) < 0) std::swap(b, c);   // PhysX's front: (b - a) x (c - a)
            PxU32 base = static_cast<PxU32>(verts.size());
            verts.push_back(a), verts.push_back(b), verts.push_back(c);
            index.push_back(base), index.push_back(base + 1), index.push_back(base + 2);
            break;
        }
        case crates::kBox:
            if (v[3] > 0 && v[4] > 0 && v[5] > 0)
                PxRigidActorExt::createExclusiveShape(*ground, PxBoxGeometry(v[3], v[4], v[5]), *g_material)
                    ->setLocalPose(PxTransform(PxVec3(v[0], v[1], v[2]), AxesQuat(v + 6)));
            break;
        case crates::kSphere:
            if (v[3] > 0)
                PxRigidActorExt::createExclusiveShape(*ground, PxSphereGeometry(v[3]), *g_material)->setLocalPose(PxTransform(PxVec3(v[0], v[1], v[2])));
            break;
        case crates::kPole:   // (a capsule along y: PhysX's capsules lie along x, turned a quarter about z)
            if (v[3] > 0)
                PxRigidActorExt::createExclusiveShape(*ground, PxCapsuleGeometry(v[3], tall * 0.5f), *g_material)
                    ->setLocalPose(PxTransform(PxVec3(v[0], (s.lo[1] + s.hi[1]) * 0.5f, v[1]), PxQuat(PxHalfPi, PxVec3(0, 0, 1))));
            break;
        default: break;
        }
        g_world.statics++;
    }
    if (!index.empty()) {
        PxTriangleMeshDesc md;
        md.points.count = static_cast<PxU32>(verts.size());
        md.points.stride = sizeof(PxVec3);
        md.points.data = verts.data();
        md.triangles.count = static_cast<PxU32>(index.size() / 3);
        md.triangles.stride = 3 * sizeof(PxU32);
        md.triangles.data = index.data();
        PxCookingParams cp(g_physics->getTolerancesScale());
        cp.meshPreprocessParams |= PxMeshPreprocessingFlag::eWELD_VERTICES;
        cp.meshWeldTolerance = 0.001f;
        PxTriangleMesh *mesh = PxCreateTriangleMesh(cp, md, g_physics->getPhysicsInsertionCallback());
        if (mesh) {
            PxRigidActorExt::createExclusiveShape(*ground, PxTriangleMeshGeometry(mesh), *g_material);
            g_world.triangles = mesh->getNbTriangles();
            mesh->release();   // (the shape keeps it)
        } else {
            Error("the static world's triangles were not cooked (" + std::to_string(index.size() / 3) + ")");
        }
    }
    g_world.scene->addActor(*ground);

    // the crates
    float h = s.half > 0.01f ? s.half : 0.3f;
    float density = (s.mass > 0 ? s.mass : 20) / (8 * h * h * h);
    uint32_t nb = s.bodies < crates::kMaxBodies ? s.bodies : crates::kMaxBodies;
    for (uint32_t i = 0; i < nb; i++) {
        const crates::Body &b = s.body[i];
        PxQuat q(b.q[0], b.q[1], b.q[2], b.q[3]);
        q = q.magnitudeSquared() > 0.5f ? q.getNormalized() : PxQuat(PxIdentity);
        PxRigidDynamic *d = PxCreateDynamic(*g_physics, PxTransform(PxVec3(b.p[0], b.p[1], b.p[2]), q), PxBoxGeometry(h, h, h), *g_material, density);
        if (!d) { Error("PxCreateDynamic failed"); break; }
        d->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
        d->setMaxDepenetrationVelocity(5);
        d->setLinearVelocity(PxVec3(b.v[0], b.v[1], b.v[2]));
        d->setAngularVelocity(PxVec3(b.w[0], b.w[1], b.w[2]));
        g_world.scene->addActor(*d);
        g_world.bodies.push_back(d);
        g_world.lost.push_back(0);
    }
    g_buildMs = Now() - t0;
    g_builds++;
    return true;
}

// The moving objects of this frame: new ones made kinematic bodies, gone ones taken out. Their places from the frame
// before to this one are given to the substeps (Step).
struct Move { Mover *m; PxTransform from, to; };
std::vector<Move> g_moves;
void Movers(const crates::Frame &f) {
    g_moves.clear();
    uint32_t n = f.movers < crates::kMaxMovers ? f.movers : crates::kMaxMovers;
    for (uint32_t i = 0; i < n; i++) {
        const crates::Mover &m = f.mover[i];
        PxTransform to = MoverPose(m);
        if (!to.isValid()) continue;
        auto it = g_world.movers.find(m.id);
        if (it == g_world.movers.end()) {
            PxRigidDynamic *k = nullptr;
            if (m.kind == crates::kBox && m.half[0] > 0 && m.half[1] > 0 && m.half[2] > 0)
                k = PxCreateKinematic(*g_physics, to, PxBoxGeometry(m.half[0], m.half[1], m.half[2]), *g_material, 1);
            else if (m.kind == crates::kSphere && m.half[0] > 0)
                k = PxCreateKinematic(*g_physics, to, PxSphereGeometry(m.half[0]), *g_material, 1);
            else if (m.kind == crates::kCapsule && m.half[0] > 0 && m.half[1] > 0)   // (PhysX's capsule lies along x)
                k = PxCreateKinematic(*g_physics, to, PxCapsuleGeometry(m.half[0], m.half[1]), *g_material, 1,
                                      PxTransform(PxQuat(PxHalfPi, PxVec3(0, 0, 1))));
            if (!k) continue;
            g_world.scene->addActor(*k);
            it = g_world.movers.emplace(m.id, Mover{k, to, f.frame}).first;
        }
        Mover &mv = it->second;
        PxTransform from = mv.last;
        if ((to.p - from.p).magnitude() > 5) {   // a teleport, not a move: put there, nothing swept on the way
            mv.body->setGlobalPose(to);
            from = to;
        }
        mv.last = to;
        mv.seen = f.frame;
        g_moves.push_back(Move{&mv, from, to});
    }
    for (auto it = g_world.movers.begin(); it != g_world.movers.end();) {
        if (it->second.seen != f.frame) {
            g_world.scene->removeActor(*it->second.body);
            it->second.body->release();
            it = g_world.movers.erase(it);
        } else {
            ++it;
        }
    }
}

// One frame's step: dt cut into substeps of at most 1/90 s (at most 8), the movers' targets along their way.
uint32_t Step(float dt) {
    if (dt <= 0) return 0;
    if (dt > 0.1f) dt = 0.1f;   // (a hitch: the crates do not jump a long way at once)
    uint32_t n = static_cast<uint32_t>(std::ceil(dt * 90.0f));
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    float h = dt / n;
    for (uint32_t i = 1; i <= n; i++) {
        float t = static_cast<float>(i) / n;
        for (Move &mv : g_moves)
            mv.m->body->setKinematicTarget(PxTransform(mv.from.p * (1 - t) + mv.to.p * t, PxSlerp(t, mv.from.q, mv.to.q)));
        g_world.scene->simulate(h);
        g_world.scene->fetchResults(true);
    }
    return n;
}

void Report(const crates::Frame &f, uint32_t substeps, double ms) {
    crates::Poses &p = g_poses;
    p.frame = f.frame;
    p.askQpc = f.qpc;
    p.serial = g_world.serial;
    p.stepMs = static_cast<float>(ms);
    p.substeps = substeps;
    p.awake = p.lost = 0;
    p.movers = static_cast<uint32_t>(g_world.movers.size());
    p.triangles = g_world.triangles;
    p.bodies = static_cast<uint32_t>(g_world.bodies.size());
    for (uint32_t i = 0; i < p.bodies; i++) {
        PxRigidDynamic *d = g_world.bodies[i];
        PxTransform t = d->getGlobalPose();
        if (!g_world.lost[i] && t.p.y < g_world.lowest) {   // out of the region: no longer simulated
            g_world.lost[i] = 1;
            d->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, true);
        }
        if (g_world.lost[i]) p.lost++;
        else if (!d->isSleeping()) p.awake++;
        PxVec3 v = g_world.lost[i] ? PxVec3(0) : d->getLinearVelocity(), w = g_world.lost[i] ? PxVec3(0) : d->getAngularVelocity();
        crates::Body &b = p.body[i];
        b = crates::Body{{t.p.x, t.p.y, t.p.z}, {t.q.x, t.q.y, t.q.z, t.q.w}, {v.x, v.y, v.z}, {w.x, w.y, w.z}};
    }
    LARGE_INTEGER done;
    QueryPerformanceCounter(&done);
    p.doneQpc = done.QuadPart;
    H->channel_write(g_posesCh, &p, static_cast<uint32_t>(offsetof(crates::Poses, body) + p.bodies * sizeof(crates::Body)));
}

// What the worker does for one new frame (C++: its objects need unwinding, so the guard is the function below).
void Work() {
    uint64_t v = 0;
    if (H->channel_read(g_frameCh, &g_frame, sizeof g_frame, &v) < offsetof(crates::Frame, mover)) return;
    static uint64_t seen;
    if (v == seen) return;
    seen = v;
    if (g_frame.serial != g_world.serial) {   // a new pile: its setup is in the channel already (written before the frame)
        uint64_t sv = 0;
        uint32_t got = H->channel_read(g_setupCh, g_setup.get(), sizeof(crates::Setup), &sv);
        if (got >= offsetof(crates::Setup, body)) {
            if (g_setup->bodies) Build(*g_setup);
            else Clear(), g_world.serial = g_setup->serial;   // (no pile: an empty world)
        }
    }
    if (!g_world.scene) return;
    double t0 = Now();
    uint32_t substeps = 0;
    if (g_frame.run && g_frame.serial == g_world.serial) {
        Movers(g_frame);
        substeps = Step(g_frame.dt);
    }
    double ms = Now() - t0;
    if (substeps) {
        g_steps++;
        g_lastMs = ms;
        g_sumMs = g_sumMs + ms;
        if (ms > g_maxMs) g_maxMs = ms;
    }
    Report(g_frame, substeps, ms);
}

int GuardedWork() {
    __try {
        Work();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<int>(GetExceptionCode());
    }
}

void Worker() {
    while (!g_stop) {
        WaitForSingleObject(g_wake, 100);
        if (g_stop) break;
        if (g_broken) continue;
        if (int code = GuardedWork()) {
            g_broken = true;
            char b[128];
            snprintf(b, sizeof b, "a fault (0x%08X) in the step: stepping stopped (mod.reload starts again)", static_cast<unsigned>(code));
            Error(b);
        }
    }
}

void OnFrame(MmlSideMod *, const char *, const char *, void *) {
    SetEvent(g_wake);
    std::string log;
    {
        std::lock_guard<std::mutex> lk(g_errMx);
        log.swap(g_toLog);
    }
    if (!log.empty()) H->log(M, 2 /* MML_LOG_WARN */, "%s", log.c_str());
}

int Side(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    std::string e;
    {
        std::lock_guard<std::mutex> lk(g_errMx);
        e = g_error;
    }
    for (char &c : e) if (c == '"' || c == '\\' || c == '\n') c = '\'';
    uint64_t steps = g_steps;
    char b[1024];
    snprintf(b, sizeof b,
             "{\"steps\": %llu, \"step_ms\": {\"last\": %.3f, \"mean\": %.3f, \"max\": %.3f}, \"bodies\": %u, \"awake\": %u, "
             "\"lost\": %u, \"movers\": %u, \"triangles\": %u, \"statics\": %u, \"builds\": %llu, \"build_ms\": %.3f, "
             "\"broken\": %s, \"physx\": \"%d.%d.%d\", \"error\": \"%s\"}",
             static_cast<unsigned long long>(steps), g_lastMs.load(), steps ? g_sumMs.load() / steps : 0.0, g_maxMs.load(), g_poses.bodies,
             g_poses.awake, g_poses.lost, g_poses.movers, g_poses.triangles, g_world.statics, static_cast<unsigned long long>(g_builds.load()),
             g_buildMs.load(), g_broken ? "true" : "false", PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX, e.c_str());
    H->reply_append(r, b, strlen(b));
    return 0;
}

int Fault(MmlSideMod *, const char *, MmlSideReply *, void *) {
    volatile int *nothing = nullptr;
    *nothing = 1;   // (on purpose: the sidecar unloads this half, the game and the 32-bit half go on)
    return 0;
}

void Shutdown() {
    if (g_worker.joinable()) {
        g_stop = true;
        SetEvent(g_wake);
        g_worker.join();
    }
    Clear();
    Release(g_material);
    Release(g_dispatcher);
    Release(g_physics);
    Release(g_foundation);
    if (g_wake) CloseHandle(g_wake);
    g_wake = nullptr;
}
} // namespace

MML_EXPORT uint32_t mml_side_api(void) { return MML_SIDE_API_VERSION; }

MML_EXPORT int mml_side_load(const MmlSideHost *host, MmlSideMod *mod) {
    H = host;
    M = mod;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_setupCh = H->channel_open(mod, "setup", sizeof(crates::Setup));
    g_frameCh = H->channel_open(mod, "frame", sizeof(crates::Frame));
    g_posesCh = H->channel_open(mod, "poses", sizeof(crates::Poses));
    if (!g_setupCh || !g_frameCh || !g_posesCh) return 1;
    // PhysX, statically linked into this DLL: its foundation is this load's own (one a process for each copy of PhysX)
    g_foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_alloc, g_errors);
    if (!g_foundation) return 2;
    g_physics = PxCreatePhysics(PX_PHYSICS_VERSION, *g_foundation, PxTolerancesScale(), false, nullptr);
    g_dispatcher = PxDefaultCpuDispatcherCreate(2);
    if (!g_physics || !g_dispatcher) { Shutdown(); return 3; }
    g_material = g_physics->createMaterial(0.6f, 0.5f, 0.15f);   // (wood on stone: static and dynamic friction, a little bounce)
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop = false;
    g_broken = false;
    g_worker = std::thread(Worker);
    H->subscribe(mod, "FRAME", OnFrame, nullptr);
    H->command(mod, "side", "{} -> what the 64-bit half has stepped: steps, step_ms, bodies, awake, movers, triangles, builds, error", Side, nullptr);
    H->command(mod, "fault", "{} -> a fault on purpose in the 64-bit half (it is unloaded; mod.reload brings it back)", Fault, nullptr);
    H->log(mod, 1 /* MML_LOG_INFO */, "PhysX %d.%d.%d, 2 worker threads", PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX);
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) { Shutdown(); }
