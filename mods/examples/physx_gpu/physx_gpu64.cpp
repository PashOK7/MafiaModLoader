// physx_gpu's 64-bit half (sdk/include/mml/side.h; the 32-bit half: physx_gpu.cpp; the records: physx_gpu.h;
// docs/MODDING.md "64-bit halves"). What a 32-bit game cannot have: PhysX 5's GPU part (CUDA) in the game's world.
//   water   a block of PBD fluid particles (tens of thousands), poured where the player looks: it runs over the
//           street, round the kerbs, the people and the cars;
//   cloth   a sheet of FEM cloth (PxDeformableSurface) dropped over whatever is there (a car, the player);
//   jelly   a soft body (PxDeformableVolume: a tetrahedral mesh) dropped, squashed by cars.
// Everything collides with the game's own static collision around it (world_shapes: its triangles cooked into a GPU
// mesh, its boxes, spheres and poles) and is pushed by the people and cars there (their collision shapes as kinematic
// bodies). The scene runs on the GPU (GPU dynamics, GPU broad phase, TGS), about an origin of its own (the city's
// coordinates are thousands of metres: PhysX works near zero).
//
// Each FRAME wakes the worker (a thread of this half's): it applies what was asked ("ops"), moves the movers ("frame"),
// steps the scene, and copies the frame's points (the water's particles, the cloth's and the jelly's vertices) from
// PhysX's device memory straight into a shared Direct3D 12 texture (CUDA external memory: nothing goes through the
// CPU), then names the texture in the channel "out". Two textures, written in turn (the 32-bit half reads the other).
//
// PhysX's GPU part is PhysXGpu.dll beside this DLL (CMake mml_physx_gpu): this half loads it by its full path (a
// wide one: the game may be in C:\Игры\Mafia), and PhysX, told its name by a load hook, finds it loaded.
// PhysX's callbacks and threads never call the host: errors are kept, logged on the mod thread and given in "out".
//   call physx_gpu.side  -> {"cuda", "gpu", "steps", "step_ms", "copy_ms", "fluid", "cloth", "jelly", "statics",
//                            "triangles", "movers", "textures", "error"}: what this half has done
//   call physx_gpu.fault -> a fault on purpose on the sidecar's mod thread (the half is unloaded; mod.reload)
#include <windows.h>
#include <d3d12.h>
#include <cuda.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <PxPhysicsAPI.h>
#include <extensions/PxParticleExt.h>
#include <extensions/PxCudaHelpersExt.h>
#include <extensions/PxDeformableSurfaceExt.h>
#include <extensions/PxDeformableVolumeExt.h>
#include <extensions/PxRemeshingExt.h>
#include <gpu/PxGpu.h>
#include "mml/side.h"
#include "physx_gpu.h"

using namespace physx;

namespace {
const MmlSideHost *H;
MmlSideMod *M;
MmlSideChannel *g_opsCh, *g_frameCh, *g_outCh, *g_topoCh;

// ---- errors: kept from any thread, logged on the mod thread
std::mutex g_errMx;
std::string g_error, g_toLog;
void Error(const std::string &e) {
    std::lock_guard<std::mutex> lk(g_errMx);
    g_error = e;
    if (g_toLog.size() < 4096) g_toLog += e + "\n";
}
std::string LastError() {
    std::lock_guard<std::mutex> lk(g_errMx);
    return g_error;
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
bool Cu(CUresult r, const char *what) {
    if (r == CUDA_SUCCESS) return true;
    const char *s = nullptr;
    cuGetErrorString(r, &s);
    Error(std::string("CUDA ") + what + ": " + (s ? s : std::to_string(r)));
    return false;
}

// ---- PhysX
class GpuLoadHook : public PxGpuLoadHook {
public:
    const char *getPhysXGpuDllName() const override { return "PhysXGpu.dll"; }   // (loaded already: GetModuleHandle)
};
GpuLoadHook g_gpuHook;
HMODULE g_gpuDll;
PxDefaultAllocator g_alloc;
ErrorCallback g_errors;
PxFoundation *g_foundation;
PxPhysics *g_physics;
PxCudaContextManager *g_cuda;
PxDefaultCpuDispatcher *g_dispatcher;
PxScene *g_scene;
PxMaterial *g_material;
CUstream g_stream;
std::string g_gpuName;

// the scene's content
PxVec3 g_origin(0);
bool g_haveOrigin;
uint32_t g_region;
PxRigidStatic *g_ground;
uint32_t g_statics, g_triangles;
PxPBDParticleSystem *g_fluid;
PxPBDMaterial *g_fluidMat;
uint32_t g_fluidPhase;
float g_spacing = 0.1f;
std::vector<PxParticleBuffer *> g_fluidBuffers;
uint32_t g_fluidCount;
PxDeformableSurface *g_cloth;
PxDeformableSurfaceMaterial *g_clothMat;
uint32_t g_clothVerts;
PxDeformableVolume *g_jelly;
PxDeformableVolumeMaterial *g_jellyMat;
uint32_t g_jellyVerts;
std::unique_ptr<gpux::Topology> g_topo = std::make_unique<gpux::Topology>();
uint32_t g_applied;   // the last Op::serial done

struct Mover {
    PxRigidDynamic *body;
    PxTransform last;
    uint64_t seen;
};
std::unordered_map<uint64_t, Mover> g_movers;   // (the worker's)
std::atomic<uint32_t> g_moverCount;

// ---- the shared textures and their CUDA mappings
struct Tex {
    ID3D12Resource *res = nullptr;   // the sidecar's
    HANDLE handle = nullptr;         // its NT handle (the sidecar's)
    uint32_t gen = 0;                // bumped by the mod thread when made
    uint32_t mappedGen = 0;          // the worker's mapping is of this one
    CUexternalMemory ext = nullptr;
    CUmipmappedArray mm = nullptr;
    CUarray arr = nullptr;
};
Tex g_tex[2];
std::mutex g_texMx;
std::atomic<bool> g_texMade;
const char *kTexNames[2] = {"pos0", "pos1"};

// ---- the worker
std::thread g_worker;
HANDLE g_wake;
std::atomic<bool> g_stop, g_broken, g_ready;
std::atomic<uint64_t> g_steps;
std::atomic<double> g_stepMs, g_copyMs, g_stepSum, g_copySum;
double g_freqMs;
std::unique_ptr<gpux::Ops> g_ops = std::make_unique<gpux::Ops>();
gpux::Frame g_frame;
gpux::Out g_out;
uint32_t g_written = 1;   // the texture last written

template <class T> void Release(T *&p) { if (p) { p->release(); p = nullptr; } }
double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_freqMs;
}
PxVec3 Local(const float *w) { return PxVec3(w[0], w[1], w[2]) - g_origin; }

PxQuat AxesQuat(const float *a) {   // (as physx_crates: a box's axes made a right-handed rotation)
    PxVec3 x(a[0], a[1], a[2]), y(a[3], a[4], a[5]), z(a[6], a[7], a[8]);
    x.normalizeSafe();
    y = (y - x * x.dot(y)).getNormalized();
    if (y.isZero()) return PxQuat(PxIdentity);
    if (x.cross(y).dot(z) < 0) y = -y;
    z = x.cross(y);
    PxQuat q(PxMat33(x, y, z));
    q.normalize();
    return q;
}

// ---- the static world of the region (world coordinates in the record, local in PhysX)
void Statics(const gpux::Ops &ops) {
    if (g_ground) {
        g_scene->removeActor(*g_ground);
        Release(g_ground);
    }
    g_ground = g_physics->createRigidStatic(PxTransform(PxIdentity));
    std::vector<PxVec3> verts;
    std::vector<PxU32> index;
    uint32_t n = std::min(ops.statics, gpux::kMaxStatics);
    float midY = (ops.lo[1] + ops.hi[1]) * 0.5f - g_origin.y, tall = ops.hi[1] - ops.lo[1] + 40;
    for (uint32_t i = 0; i < n; i++) {
        const float *v = ops.shape[i].v;
        switch (ops.shape[i].kind) {
        case gpux::kTriangle: {
            PxVec3 a = Local(v), b = Local(v + 3), c = Local(v + 6), nrm(v[9], v[10], v[11]);
            if ((b - a).cross(c - a).dot(nrm) < 0) std::swap(b, c);
            PxU32 base = static_cast<PxU32>(verts.size());
            verts.push_back(a), verts.push_back(b), verts.push_back(c);
            index.push_back(base), index.push_back(base + 1), index.push_back(base + 2);
            break;
        }
        case gpux::kBox:
            if (v[3] > 0 && v[4] > 0 && v[5] > 0)
                PxRigidActorExt::createExclusiveShape(*g_ground, PxBoxGeometry(v[3], v[4], v[5]), *g_material)
                    ->setLocalPose(PxTransform(Local(v), AxesQuat(v + 6)));
            break;
        case gpux::kSphere:
            if (v[3] > 0) PxRigidActorExt::createExclusiveShape(*g_ground, PxSphereGeometry(v[3]), *g_material)->setLocalPose(PxTransform(Local(v)));
            break;
        case gpux::kPole:
            if (v[3] > 0)
                PxRigidActorExt::createExclusiveShape(*g_ground, PxCapsuleGeometry(v[3], tall * 0.5f), *g_material)
                    ->setLocalPose(PxTransform(PxVec3(v[0] - g_origin.x, midY, v[1] - g_origin.z), PxQuat(PxHalfPi, PxVec3(0, 0, 1))));
            break;
        default: break;
        }
    }
    g_statics = n;
    g_triangles = 0;
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
        cp.buildGPUData = true;   // (the GPU's narrow phase: particles, cloth, soft bodies against it)
        cp.midphaseDesc = PxMeshMidPhase::eBVH34;
        if (PxTriangleMesh *mesh = PxCreateTriangleMesh(cp, md, g_physics->getPhysicsInsertionCallback())) {
            PxRigidActorExt::createExclusiveShape(*g_ground, PxTriangleMeshGeometry(mesh), *g_material);
            g_triangles = mesh->getNbTriangles();
            mesh->release();
        } else {
            Error("the static world's triangles were not cooked");
        }
    }
    g_scene->addActor(*g_ground);
}

// ---- water: one PBD particle system, a buffer of particles each pour
void Pour(const gpux::Op &op) {
    if (!g_fluid) {
        g_spacing = op.param > 0.02f ? op.param : 0.1f;
        float rest = 0.5f * g_spacing / 0.6f;
        g_fluidMat = g_physics->createPBDMaterial(0.05f, 0.05f, 0.f, 0.001f, 0.5f, 0.005f, 0.01f, 0.f, 0.f);
        g_fluidMat->setViscosity(0.001f);
        g_fluidMat->setSurfaceTension(0.00704f);
        g_fluidMat->setCohesion(0.0704f);
        g_fluidMat->setVorticityConfinement(10.f);
        g_fluid = g_physics->createPBDParticleSystem(*g_cuda, 96);
        if (!g_fluid) { Error("createPBDParticleSystem failed"); return; }
        g_fluid->setRestOffset(rest);
        g_fluid->setContactOffset(rest + 0.01f);
        g_fluid->setParticleContactOffset(rest);
        g_fluid->setSolidRestOffset(rest);
        g_fluid->setFluidRestOffset(rest * 0.6f);
        g_fluid->setMaxLinearVelocity(rest * 100.f);
        // (the game's ground is a triangle mesh, one-sided and thin, where NVIDIA's snippet has planes: a particle
        // that crosses a face in a step is under it for good. Speculative CCD keeps a fast particle on its side, and
        // more position iterations hold a deep body of water up on it)
        g_fluid->setParticleFlag(PxParticleFlag::eENABLE_SPECULATIVE_CCD, true);
        g_fluid->setSolverIterationCounts(8, 1);
        g_scene->addActor(*g_fluid);
        g_fluidPhase = g_fluid->createPhase(g_fluidMat, PxParticlePhaseFlags(PxParticlePhaseFlag::eParticlePhaseFluid | PxParticlePhaseFlag::eParticlePhaseSelfCollide));
    }
    uint32_t want = std::min(op.count ? op.count : 32768u, gpux::kMaxFluid - g_fluidCount);
    uint32_t k = static_cast<uint32_t>(std::cbrt(static_cast<double>(want)));
    if (k < 2) { Error("the water is full (" + std::to_string(g_fluidCount) + " particles)"); return; }
    uint32_t n = k * k * k;
    float s = g_spacing, mass = 1000.f * 1.333f * 3.14159f * s * s * s;   // (the snippet's: a particle's mass at water's density)
    PxVec3 c = Local(op.at), corner = c - PxVec3(0.5f * s * (k - 1), 0, 0.5f * s * (k - 1));
    PxU32 *phase = PX_EXT_PINNED_MEMORY_ALLOC(PxU32, *g_cuda, n);
    PxVec4 *pos = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, *g_cuda, n);
    PxVec4 *vel = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, *g_cuda, n);
    uint32_t i = 0;
    for (uint32_t x = 0; x < k; x++)
        for (uint32_t y = 0; y < k; y++)
            for (uint32_t z = 0; z < k; z++, i++) {
                pos[i] = PxVec4(corner + PxVec3(x * s, y * s, z * s), 1.0f / mass);
                vel[i] = PxVec4(PxZero);
                phase[i] = g_fluidPhase;
            }
    ExtGpu::PxParticleBufferDesc d;
    d.maxParticles = n;
    d.numActiveParticles = n;
    d.positions = pos;
    d.velocities = vel;
    d.phases = phase;
    PxParticleBuffer *b = ExtGpu::PxCreateAndPopulateParticleBuffer(d, g_cuda);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, pos);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, vel);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, phase);
    if (!b) { Error("the particle buffer was not made"); return; }
    g_fluid->addParticleBuffer(b);
    g_fluidBuffers.push_back(b);
    g_fluidCount += n;
}

// ---- the topology the 32-bit half draws: the cloth's triangles, then the jelly's surface
std::vector<uint32_t> g_clothTris, g_jellyTris;
void SendTopology() {
    gpux::Topology &t = *g_topo;
    t.version++;
    t.clothTris = static_cast<uint32_t>(std::min<size_t>(g_clothTris.size() / 3, gpux::kMaxClothTris));
    t.jellyTris = static_cast<uint32_t>(std::min<size_t>(g_jellyTris.size() / 3, gpux::kMaxJellyTris));
    memcpy(t.index, g_clothTris.data(), t.clothTris * 12);
    memcpy(t.index + 3 * t.clothTris, g_jellyTris.data(), t.jellyTris * 12);
    H->channel_write(g_topoCh, &t, static_cast<uint32_t>(offsetof(gpux::Topology, index) + 12 * (t.clothTris + t.jellyTris)));
}

void ReleaseCloth() {
    if (g_cloth) {
        g_scene->removeActor(*g_cloth);
        Release(g_cloth);
    }
    g_clothVerts = 0;
    g_clothTris.clear();
}
void ReleaseJelly() {
    if (g_jelly) {
        g_scene->removeActor(*g_jelly);
        Release(g_jelly);
    }
    g_jellyVerts = 0;
    g_jellyTris.clear();
}
void ReleaseFluid() {
    for (PxParticleBuffer *b : g_fluidBuffers) {
        if (g_fluid) g_fluid->removeParticleBuffer(b);
        b->release();
    }
    g_fluidBuffers.clear();
    g_fluidCount = 0;
    if (g_fluid) {
        g_scene->removeActor(*g_fluid);
        Release(g_fluid);
    }
    Release(g_fluidMat);
}

// ---- cloth: a square sheet of n x n vertices, `size` metres a side, level, its middle at the op's place
void Cloth(const gpux::Op &op) {
    ReleaseCloth();
    uint32_t n = std::clamp(op.count ? op.count : 64u, 8u, 128u);
    float size = op.size > 0.5f ? op.size : 4.f, step = size / (n - 1), thickness = 0.01f, density = 100.f;
    PxVec3 c = Local(op.at);
    std::vector<PxVec3> verts, vel(n * n, PxVec3(0));
    std::vector<PxU32> tris;
    std::vector<PxReal> masses;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < n; j++) verts.push_back(c + PxVec3((i - 0.5f * (n - 1)) * step, 0, (j - 0.5f * (n - 1)) * step));
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = 1; j < n; j++) {
            PxU32 a = (i - 1) * n + (j - 1), b = i * n + (j - 1), d = (i - 1) * n + j, e = i * n + j;
            PxU32 t[6] = {a, b, d, d, b, e};
            for (int k = 0; k < 2; k++) {
                tris.insert(tris.end(), t + 3 * k, t + 3 * k + 3);
                const PxU32 *q = t + 3 * k;
                masses.push_back(0.5f * (verts[q[1]] - verts[q[0]]).cross(verts[q[2]] - verts[q[0]]).magnitude() * thickness * density);
            }
        }
    PxCookingParams cp(g_physics->getTolerancesScale());
    cp.meshWeldTolerance = 0.001f;
    cp.meshPreprocessParams = PxMeshPreprocessingFlags(PxMeshPreprocessingFlag::eWELD_VERTICES) | PxMeshPreprocessingFlag::eENABLE_VERT_MAPPING;
    cp.buildTriangleAdjacencies = false;
    cp.buildGPUData = true;
    cp.midphaseDesc = PxMeshMidPhase::eBVH34;
    PxTriangleMeshDesc md;
    md.points.count = static_cast<PxU32>(verts.size());
    md.points.stride = sizeof(PxVec3);
    md.points.data = verts.data();
    md.triangles.count = static_cast<PxU32>(tris.size() / 3);
    md.triangles.stride = 3 * sizeof(PxU32);
    md.triangles.data = tris.data();
    PxTriangleMesh *mesh = PxCreateTriangleMesh(cp, md, g_physics->getPhysicsInsertionCallback());
    if (!mesh) { Error("the cloth's mesh was not cooked"); return; }
    if (!g_clothMat) g_clothMat = g_physics->createDeformableSurfaceMaterial(1.e10f, 0.3f, 0.5f, thickness, 1.e-5f);
    g_cloth = g_physics->createDeformableSurface(*g_cuda);
    if (!g_cloth) { mesh->release(); Error("createDeformableSurface failed"); return; }
    PxShape *shape = g_physics->createShape(PxTriangleMeshGeometry(mesh), &g_clothMat, 1, true,
                                            PxShapeFlag::eSCENE_QUERY_SHAPE | PxShapeFlag::eSIMULATION_SHAPE);
    if (shape) g_cloth->attachShape(*shape);
    g_scene->addActor(*g_cloth);
    PxVec4 *posPinned, *velPinned, *restPinned;
    PxDeformableSurfaceExt::allocateAndInitializeHostMirror(*g_cloth, verts.data(), vel.data(), verts.data(), 0.5f, PxTransform(PxIdentity), g_cuda,
                                                            posPinned, velPinned, restPinned);
    PxDeformableSurfaceExt::distributeTriangleMassToVertices(*g_cloth, masses.data(), posPinned);
    if (shape) {
        shape->setContactOffset(2.0f * thickness);
        shape->setRestOffset(thickness);
        shape->release();
    }
    PxDeformableSurfaceExt::copyToDevice(*g_cloth, PxDeformableSurfaceDataFlag::eALL, static_cast<PxU32>(verts.size()), posPinned, velPinned, restPinned);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, posPinned);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, velPinned);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, restPinned);
    g_cloth->setSelfCollisionFilterDistance(thickness * 2.5f);
    g_cloth->setDeformableBodyFlag(PxDeformableBodyFlag::eDISABLE_SELF_COLLISION, false);
    g_cloth->setMaxLinearVelocity(100.0f);
    // what is drawn: the cooked mesh's vertices (the device's order) and its triangles
    g_clothVerts = std::min(mesh->getNbVertices(), gpux::kMaxCloth);
    const bool i16 = mesh->getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES;
    for (PxU32 t = 0; t < mesh->getNbTriangles() * 3; t++)
        g_clothTris.push_back(i16 ? static_cast<const PxU16 *>(mesh->getTriangles())[t] : static_cast<const PxU32 *>(mesh->getTriangles())[t]);
    mesh->release();
    SendTopology();
}

// ---- jelly: a cube's surface made a tetrahedral soft body; its collision mesh's surface is what is drawn
void Jelly(const gpux::Op &op) {
    ReleaseJelly();
    float size = op.size > 0.2f ? op.size : 1.5f, h = size * 0.5f;
    PxArray<PxVec3> v;
    PxArray<PxU32> t;
    const PxVec3 corners[8] = {{h, -h, -h}, {h, -h, h}, {-h, -h, h}, {-h, -h, -h}, {h, h, -h}, {h, h, h}, {-h, h, h}, {-h, h, -h}};
    for (const PxVec3 &p : corners) v.pushBack(p);
    const PxU32 faces[36] = {1, 2, 3, 7, 6, 5, 4, 5, 1, 5, 6, 2, 2, 6, 7, 0, 3, 7, 0, 1, 3, 4, 7, 5, 0, 4, 1, 1, 5, 2, 3, 2, 7, 4, 0, 7};
    for (PxU32 f : faces) t.pushBack(f);
    PxRemeshingExt::limitMaxEdgeLength(t, v, size / 5);
    PxSimpleTriangleMesh surface;
    surface.points.count = v.size();
    surface.points.stride = sizeof(PxVec3);
    surface.points.data = v.begin();
    surface.triangles.count = t.size() / 3;
    surface.triangles.stride = 3 * sizeof(PxU32);
    surface.triangles.data = t.begin();
    PxCookingParams cp(g_physics->getTolerancesScale());
    cp.meshWeldTolerance = 0.001f;
    cp.meshPreprocessParams = PxMeshPreprocessingFlags(PxMeshPreprocessingFlag::eWELD_VERTICES);
    cp.buildTriangleAdjacencies = false;
    cp.buildGPUData = true;
    PxDeformableVolumeMesh *vm = PxDeformableVolumeExt::createDeformableVolumeMesh(cp, surface, 10, g_physics->getPhysicsInsertionCallback());
    if (!vm) { Error("the jelly's tetrahedral mesh was not made"); return; }
    if (!g_jellyMat) g_jellyMat = g_physics->createDeformableVolumeMaterial(op.param > 0 ? op.param : 5.e4f, 0.45f, 0.5f);
    g_jelly = g_physics->createDeformableVolume(*g_cuda);
    if (!g_jelly) { Error("createDeformableVolume failed"); return; }
    PxTetrahedronMeshGeometry geom(vm->getCollisionMesh());
    PxShape *shape = g_physics->createShape(geom, &g_jellyMat, 1, true,
                                            PxShapeFlag::eSCENE_QUERY_SHAPE | PxShapeFlag::eSIMULATION_SHAPE);
    if (shape) {
        g_jelly->attachShape(*shape);
        shape->release();
    }
    g_jelly->attachSimulationMesh(*vm->getSimulationMesh(), *vm->getDeformableVolumeAuxData());
    g_scene->addActor(*g_jelly);
    PxVec4 *simPos, *simVel, *collPos, *restPos;
    PxDeformableVolumeExt::allocateAndInitializeHostMirror(*g_jelly, g_cuda, simPos, simVel, collPos, restPos);
    PxDeformableVolumeExt::transform(*g_jelly, PxTransform(Local(op.at)), 1.0f, simPos, simVel, collPos, restPos);
    PxDeformableVolumeExt::updateMass(*g_jelly, 400.f, 50.f, simPos);
    PxDeformableVolumeExt::copyToDevice(*g_jelly, PxDeformableVolumeDataFlag::eALL, simPos, simVel, collPos, restPos);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, simPos);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, simVel);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, collPos);
    PX_EXT_PINNED_MEMORY_FREE(*g_cuda, restPos);
    g_jelly->setDeformableBodyFlag(PxDeformableBodyFlag::eDISABLE_SELF_COLLISION, true);
    g_jelly->setSolverIterationCounts(30);
    // the surface drawn: the collision mesh's faces that belong to one tetrahedron alone, turned away from its fourth
    // vertex (the device's positions are the collision mesh's vertices, in its order)
    const PxTetrahedronMesh *cm = vm->getCollisionMesh();
    g_jellyVerts = std::min(cm->getNbVertices(), gpux::kMaxJelly);
    const PxVec3 *rest = cm->getVertices();
    const bool i16 = cm->getTetrahedronMeshFlags() & PxTetrahedronMeshFlag::e16_BIT_INDICES;
    auto at = [&](PxU32 k) -> PxU32 { return i16 ? static_cast<const PxU16 *>(cm->getTetrahedrons())[k] : static_cast<const PxU32 *>(cm->getTetrahedrons())[k]; };
    std::map<uint64_t, std::pair<int, std::array<PxU32, 3>>> faces2;
    const int opp[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
    for (PxU32 k = 0; k < cm->getNbTetrahedrons(); k++) {
        PxU32 q[4] = {at(4 * k), at(4 * k + 1), at(4 * k + 2), at(4 * k + 3)};
        for (int f = 0; f < 4; f++) {
            std::array<PxU32, 3> tri = {q[opp[f][0]], q[opp[f][1]], q[opp[f][2]]};
            PxVec3 nrm = (rest[tri[1]] - rest[tri[0]]).cross(rest[tri[2]] - rest[tri[0]]);
            if (nrm.dot(rest[tri[0]] - rest[q[f]]) < 0) std::swap(tri[1], tri[2]);   // (outward: away from the fourth)
            std::array<PxU32, 3> key = tri;
            std::sort(key.begin(), key.end());
            uint64_t id = (static_cast<uint64_t>(key[0]) << 42) ^ (static_cast<uint64_t>(key[1]) << 21) ^ key[2];
            auto &e = faces2[id];
            e.first++;
            e.second = tri;
        }
    }
    for (auto &kv : faces2)
        if (kv.second.first == 1) g_jellyTris.insert(g_jellyTris.end(), kv.second.second.begin(), kv.second.second.end());
    SendTopology();
}

void Apply(const gpux::Op &op) {
    switch (op.kind) {
    case gpux::kOpPour: Pour(op); break;
    case gpux::kOpCloth: Cloth(op); break;
    case gpux::kOpJelly: Jelly(op); break;
    case gpux::kOpClear:
        ReleaseFluid();
        ReleaseCloth();
        ReleaseJelly();
        SendTopology();
        break;
    default: break;
    }
}

void Ops() {
    uint64_t v = 0;
    if (H->channel_read(g_opsCh, g_ops.get(), sizeof(gpux::Ops), &v) < offsetof(gpux::Ops, shape)) return;
    const gpux::Ops &o = *g_ops;
    if (o.region != g_region) {
        if (!g_haveOrigin) {   // (the scene's origin: the first region's centre, kept while there is anything)
            g_origin = PxVec3((o.lo[0] + o.hi[0]) * 0.5f, (o.lo[1] + o.hi[1]) * 0.5f, (o.lo[2] + o.hi[2]) * 0.5f);
            g_haveOrigin = true;
        }
        Statics(o);
        g_region = o.region;
    }
    std::vector<const gpux::Op *> todo;
    for (const gpux::Op &op : o.op)
        if (op.serial > g_applied) todo.push_back(&op);
    std::sort(todo.begin(), todo.end(), [](const gpux::Op *a, const gpux::Op *b) { return a->serial < b->serial; });
    for (const gpux::Op *op : todo) {
        Apply(*op);
        g_applied = op->serial;
    }
}

// ---- the movers (as physx_crates's, about the origin)
struct Move { Mover *m; PxTransform from, to; };
std::vector<Move> g_moves;
PxTransform MoverPose(const gpux::Mover &m) {
    PxVec3 c = Local(m.center);
    if (m.kind == gpux::kBox) return PxTransform(c, AxesQuat(m.axes));
    if (m.kind == gpux::kCapsule) return PxTransform(c, PxQuat(PxHalfPi, PxVec3(0, 0, 1)));   // (PhysX's capsule lies along x)
    return PxTransform(c);
}
void Movers(const gpux::Frame &f) {
    g_moves.clear();
    for (uint32_t i = 0; i < std::min(f.movers, gpux::kMaxMovers); i++) {
        const gpux::Mover &m = f.mover[i];
        PxTransform to = MoverPose(m);
        if (!to.isValid()) continue;
        auto it = g_movers.find(m.id);
        if (it == g_movers.end()) {
            PxRigidDynamic *k = nullptr;
            if (m.kind == gpux::kBox && m.half[0] > 0 && m.half[1] > 0 && m.half[2] > 0)
                k = PxCreateKinematic(*g_physics, to, PxBoxGeometry(m.half[0], m.half[1], m.half[2]), *g_material, 1);
            else if (m.kind == gpux::kSphere && m.half[0] > 0)
                k = PxCreateKinematic(*g_physics, to, PxSphereGeometry(m.half[0]), *g_material, 1);
            else if (m.kind == gpux::kCapsule && m.half[0] > 0 && m.half[1] > 0)
                k = PxCreateKinematic(*g_physics, to, PxCapsuleGeometry(m.half[0], m.half[1]), *g_material, 1);
            if (!k) continue;
            g_scene->addActor(*k);
            it = g_movers.emplace(m.id, Mover{k, to, f.frame}).first;
        }
        Mover &mv = it->second;
        PxTransform from = mv.last;
        if ((to.p - from.p).magnitude() > 5) {
            mv.body->setGlobalPose(to);
            from = to;
        }
        mv.last = to;
        mv.seen = f.frame;
        g_moves.push_back(Move{&mv, from, to});
    }
    for (auto it = g_movers.begin(); it != g_movers.end();) {
        if (it->second.seen != f.frame) {
            g_scene->removeActor(*it->second.body);
            it->second.body->release();
            it = g_movers.erase(it);
        } else {
            ++it;
        }
    }
    g_moverCount = static_cast<uint32_t>(g_movers.size());
}

uint32_t Step(float dt) {
    if (dt <= 0) return 0;
    dt = std::min(dt, 1 / 30.f);
    uint32_t n = std::clamp(static_cast<uint32_t>(std::ceil(dt * 60.0f - 0.01f)), 1u, 2u);
    float h = dt / n;
    for (uint32_t i = 1; i <= n; i++) {
        float t = static_cast<float>(i) / n;
        for (Move &mv : g_moves)
            mv.m->body->setKinematicTarget(PxTransform(mv.from.p * (1 - t) + mv.to.p * t, PxSlerp(t, mv.from.q, mv.to.q)));
        g_scene->simulate(h);
        g_scene->fetchResults(true);
        g_scene->fetchResultsParticleSystem();
    }
    return n;
}

// ---- the points into a texture: a run of n points from device memory to point `first` of the texture (rows of
// kPointsPerRow points, 16 bytes each)
bool CopyRun(CUarray arr, CUdeviceptr src, uint32_t n, uint32_t first) {
    const size_t rowBytes = gpux::kTexWidth * 4;
    while (n) {
        uint32_t row = first / gpux::kPointsPerRow, col = first % gpux::kPointsPerRow;
        uint32_t take = col ? std::min(n, gpux::kPointsPerRow - col) : n;
        CUDA_MEMCPY2D c{};
        c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        c.srcDevice = src;
        c.srcPitch = rowBytes;
        c.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        c.dstArray = arr;
        c.dstXInBytes = col * 16;
        c.dstY = row;
        if (col || take < gpux::kPointsPerRow) {   // (a part of one row)
            take = std::min(take, gpux::kPointsPerRow - col);
            c.WidthInBytes = take * 16;
            c.Height = 1;
        } else {   // whole rows
            uint32_t rows = take / gpux::kPointsPerRow;
            take = rows * gpux::kPointsPerRow;
            c.WidthInBytes = rowBytes;
            c.Height = rows;
        }
        if (!Cu(cuMemcpy2DAsync(&c, g_stream), "cuMemcpy2DAsync")) return false;
        src += static_cast<CUdeviceptr>(take) * 16;
        first += take;
        n -= take;
    }
    return true;
}

// The texture's CUDA mapping, made again when the mod thread has made the texture anew.
bool Map(Tex &t) {
    std::lock_guard<std::mutex> lk(g_texMx);
    if (!t.res) return false;
    if (t.mappedGen == t.gen && t.arr) return true;
    if (t.mm) cuMipmappedArrayDestroy(t.mm), t.mm = nullptr;
    if (t.ext) cuDestroyExternalMemory(t.ext), t.ext = nullptr;
    t.arr = nullptr;
    auto *dev = static_cast<ID3D12Device *>(H->texture_d3d12_device());
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &rd);
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC hd{};
    hd.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
    hd.handle.win32.handle = t.handle;
    hd.size = ai.SizeInBytes;
    hd.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
    if (!Cu(cuImportExternalMemory(&t.ext, &hd), "cuImportExternalMemory")) return false;
    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC md{};
    md.arrayDesc.Width = gpux::kTexWidth;
    md.arrayDesc.Height = gpux::kRows;
    md.arrayDesc.Depth = 0;
    md.arrayDesc.Format = CU_AD_FORMAT_FLOAT;
    md.arrayDesc.NumChannels = 1;
    md.arrayDesc.Flags = CUDA_ARRAY3D_COLOR_ATTACHMENT;   // (a render target's memory)
    md.numLevels = 1;
    if (!Cu(cuExternalMemoryGetMappedMipmappedArray(&t.mm, t.ext, &md), "cuExternalMemoryGetMappedMipmappedArray") ||
        !Cu(cuMipmappedArrayGetLevel(&t.arr, t.mm, 0), "cuMipmappedArrayGetLevel"))
        return false;
    t.mappedGen = t.gen;
    return true;
}
void Unmap(Tex &t) {
    if (t.mm) cuMipmappedArrayDestroy(t.mm), t.mm = nullptr;
    if (t.ext) cuDestroyExternalMemory(t.ext), t.ext = nullptr;
    t.arr = nullptr;
    t.mappedGen = 0;
}

// `points`: where the water's particles are, read back from the GPU by the worker after a step (asked here, measured
// at the next frame: each call answers the last measure and asks for the next)
std::mutex g_boundsMx;
std::string g_bounds = "{\"measured\": false}";
std::atomic<bool> g_wantBounds;
void MeasureBounds() {   // (the worker, the CUDA context acquired)
    std::vector<PxVec4> p;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    uint64_t nan = 0, total = 0;
    for (PxParticleBuffer *b : g_fluidBuffers) {
        uint32_t n = b->getNbActiveParticles();
        p.resize(n);
        if (!n || cuMemcpyDtoH(p.data(), reinterpret_cast<CUdeviceptr>(b->getPositionInvMasses()), n * sizeof(PxVec4)) != CUDA_SUCCESS) continue;
        for (const PxVec4 &q : p) {
            total++;
            if (!(q.x == q.x && q.y == q.y && q.z == q.z)) { nan++; continue; }
            const float v[3] = {q.x + g_origin.x, q.y + g_origin.y, q.z + g_origin.z};
            for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
        }
    }
    char b[512];
    snprintf(b, sizeof b, "{\"measured\": true, \"particles\": %llu, \"nan\": %llu, \"lo\": [%.2f, %.2f, %.2f], \"hi\": [%.2f, %.2f, %.2f], "
             "\"origin\": [%.2f, %.2f, %.2f], \"step\": %llu}", static_cast<unsigned long long>(total), static_cast<unsigned long long>(nan),
             lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], g_origin.x, g_origin.y, g_origin.z, static_cast<unsigned long long>(g_steps.load()));
    std::lock_guard<std::mutex> lk(g_boundsMx);
    g_bounds = b;
}
int Points(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    std::string s;
    {
        std::lock_guard<std::mutex> lk(g_boundsMx);
        s = g_bounds;
    }
    g_wantBounds = true;
    H->reply_append(r, s.c_str(), s.size());
    return 0;
}

// The frame's points into the texture the 32-bit half is not reading; false: none there yet.
bool Copy(uint32_t which) {
    double t0 = Now();
    g_cuda->acquireContext();
    bool ok = Map(g_tex[which]);
    if (ok) {
        CUarray arr = g_tex[which].arr;
        ok = Cu(cuCtxSynchronize(), "cuCtxSynchronize");   // (PhysX's own streams done with the step)
        uint32_t first = gpux::kFluidRow * gpux::kPointsPerRow;
        for (PxParticleBuffer *b : g_fluidBuffers) {
            if (!ok) break;
            uint32_t n = b->getNbActiveParticles();
            ok = CopyRun(arr, reinterpret_cast<CUdeviceptr>(b->getPositionInvMasses()), n, first);
            first += n;
        }
        if (ok && g_cloth && g_clothVerts)
            ok = CopyRun(arr, reinterpret_cast<CUdeviceptr>(g_cloth->getPositionInvMassBufferD()), g_clothVerts, gpux::kClothRow * gpux::kPointsPerRow);
        if (ok && g_jelly && g_jellyVerts)
            ok = CopyRun(arr, reinterpret_cast<CUdeviceptr>(g_jelly->getPositionInvMassBufferD()), g_jellyVerts, gpux::kJellyRow * gpux::kPointsPerRow);
        ok = ok && Cu(cuStreamSynchronize(g_stream), "cuStreamSynchronize");   // (whole before the channel names it)
        if (g_wantBounds.exchange(false)) MeasureBounds();
    }
    g_cuda->releaseContext();
    double ms = Now() - t0;
    g_copyMs = ms;
    g_copySum = g_copySum + ms;
    return ok;
}

void Work() {
    uint64_t v = 0;
    if (H->channel_read(g_frameCh, &g_frame, sizeof g_frame, &v) < offsetof(gpux::Frame, mover)) return;
    static uint64_t seen;
    if (v == seen) return;
    seen = v;
    if (g_frame.lastOp > g_applied) Ops();
    double t0 = Now();
    uint32_t substeps = 0;
    if (g_frame.run && g_ground) {
        Movers(g_frame);
        substeps = Step(g_frame.dt);
    }
    double stepMs = Now() - t0;
    if (substeps) {
        g_steps++;
        g_stepMs = stepMs;
        g_stepSum = g_stepSum + stepMs;
    }
    gpux::Out &o = g_out;
    o.frame = g_frame.frame;
    o.askQpc = g_frame.qpc;
    uint32_t which = g_written ^ 1;
    bool any = g_fluidCount || g_clothVerts || g_jellyVerts;
    if (any && g_texMade && Copy(which)) {
        g_written = which;
        o.texture = which;
    } else {
        o.texture = 0xFFFFFFFFu;   // (nothing drawn this frame)
    }
    o.fluid = std::min(g_fluidCount, gpux::kMaxFluid);
    o.cloth = g_clothVerts;
    o.jelly = g_jellyVerts;
    o.origin[0] = g_origin.x, o.origin[1] = g_origin.y, o.origin[2] = g_origin.z;
    o.radius = g_spacing * 0.5f;
    memcpy(o.lo, g_ops->lo, sizeof o.lo);
    memcpy(o.hi, g_ops->hi, sizeof o.hi);
    o.topology = g_topo->version;
    o.substeps = substeps;
    o.stepMs = static_cast<float>(stepMs);
    o.copyMs = static_cast<float>(g_copyMs.load());
    o.region = g_region;
    o.lastOp = g_applied;
    snprintf(o.error, sizeof o.error, "%s", LastError().c_str());
    LARGE_INTEGER done;
    QueryPerformanceCounter(&done);
    o.doneQpc = done.QuadPart;
    H->channel_write(g_outCh, &o, sizeof o);
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
        if (g_broken || !g_ready) continue;
        if (int code = GuardedWork()) {
            g_broken = true;
            char b[128];
            snprintf(b, sizeof b, "a fault (0x%08X) in the step: stepping stopped (mod.reload starts again)", static_cast<unsigned>(code));
            Error(b);
        }
    }
}

// ---- the mod thread: the textures (once the game's adapter is known), the worker woken, the errors logged
void MakeTextures() {
    if (!H->texture_d3d12_device()) return;
    for (int i = 0; i < 2; i++) {
        MmlSideTexture t{};
        t.size = sizeof t;
        if (!H->texture_create(M, kTexNames[i], gpux::kTexWidth, gpux::kRows, 41 /* DXGI_FORMAT_R32_FLOAT */, &t)) {
            Error("texture_create failed");
            return;
        }
        std::lock_guard<std::mutex> lk(g_texMx);
        g_tex[i].res = static_cast<ID3D12Resource *>(t.d3d12_resource);
        g_tex[i].handle = t.shared_handle;
        g_tex[i].gen++;
    }
    g_texMade = true;
}

void OnFrame(MmlSideMod *, const char *, const char *, void *) {
    static unsigned waited;
    if (!g_texMade && g_ready && waited++ % 60 == 0) MakeTextures();   // (once a second until the adapter is known)
    SetEvent(g_wake);
    std::string log;
    {
        std::lock_guard<std::mutex> lk(g_errMx);
        log.swap(g_toLog);
    }
    if (!log.empty()) H->log(M, 2, "%s", log.c_str());
}

int Side(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    std::string e = LastError();
    for (char &c : e) if (c == '"' || c == '\\' || c == '\n') c = '\'';
    uint64_t steps = g_steps;
    char b[1536];
    snprintf(b, sizeof b,
             "{\"cuda\": %s, \"gpu\": \"%s\", \"steps\": %llu, \"step_ms\": {\"last\": %.3f, \"mean\": %.3f}, \"copy_ms\": {\"last\": %.3f, "
             "\"mean\": %.3f}, \"fluid\": %u, \"cloth\": %u, \"jelly\": %u, \"statics\": %u, \"triangles\": %u, \"movers\": %u, "
             "\"textures\": %s, \"broken\": %s, \"physx\": \"%d.%d.%d\", \"error\": \"%s\"}",
             g_ready ? "true" : "false", g_gpuName.c_str(), static_cast<unsigned long long>(steps), g_stepMs.load(), steps ? g_stepSum.load() / steps : 0.0,
             g_copyMs.load(), steps ? g_copySum.load() / steps : 0.0, g_fluidCount, g_clothVerts, g_jellyVerts, g_statics, g_triangles,
             g_moverCount.load(), g_texMade ? "true" : "false", g_broken ? "true" : "false", PX_PHYSICS_VERSION_MAJOR,
             PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX, e.c_str());
    H->reply_append(r, b, strlen(b));
    return 0;
}

int Fault(MmlSideMod *, const char *, MmlSideReply *, void *) {
    volatile int *nothing = nullptr;
    *nothing = 1;   // (on purpose: the sidecar unloads this half, the game and the 32-bit half go on)
    return 0;
}

// PhysX on the GPU: the CUDA context manager (PhysXGpu.dll beside this DLL), a scene with GPU dynamics.
bool StartPhysX() {
    g_foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_alloc, g_errors);
    if (!g_foundation) return false;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&StartPhysX), &self);
    wchar_t path[MAX_PATH * 4];
    std::wstring dll(path, GetModuleFileNameW(self, path, MAX_PATH * 4));
    dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"PhysXGpu.dll";
    g_gpuDll = LoadLibraryW(dll.c_str());
    if (!g_gpuDll) {
        Error("PhysXGpu.dll was not loaded from beside the half (" + std::to_string(GetLastError()) + ")");
        return false;
    }
    PxSetPhysXGpuLoadHook(&g_gpuHook);
    PxCudaContextManagerDesc cd;
    g_cuda = PxCreateCudaContextManager(*g_foundation, cd, nullptr);
    if (!g_cuda || !g_cuda->contextIsValid()) {
        Error("no CUDA context: PhysX's GPU part needs an NVIDIA GPU (Volta or newer) and PhysXGpu.dll beside the half");
        Release(g_cuda);
        return false;
    }
    g_gpuName = g_cuda->getDeviceName() ? g_cuda->getDeviceName() : "";
    g_physics = PxCreatePhysics(PX_PHYSICS_VERSION, *g_foundation, PxTolerancesScale(), false, nullptr);
    g_dispatcher = PxDefaultCpuDispatcherCreate(2);
    if (!g_physics || !g_dispatcher) return false;
    PxSceneDesc sd(g_physics->getTolerancesScale());
    sd.gravity = PxVec3(0, -9.81f, 0);
    sd.cpuDispatcher = g_dispatcher;
    sd.filterShader = PxDefaultSimulationFilterShader;
    sd.cudaContextManager = g_cuda;
    sd.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS | PxSceneFlag::eENABLE_PCM;
    sd.broadPhaseType = PxBroadPhaseType::eGPU;
    sd.solverType = PxSolverType::eTGS;
    sd.gpuMaxNumPartitions = 8;
    g_scene = g_physics->createScene(sd);
    if (!g_scene) return false;
    g_material = g_physics->createMaterial(0.5f, 0.5f, 0.1f);
    g_cuda->acquireContext();
    bool ok = Cu(cuStreamCreate(&g_stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
    g_cuda->releaseContext();
    return ok;
}

void Shutdown() {
    if (g_worker.joinable()) {
        g_stop = true;
        SetEvent(g_wake);
        g_worker.join();
    }
    if (g_cuda) {
        g_cuda->acquireContext();
        for (Tex &t : g_tex) Unmap(t);
        if (g_stream) cuStreamDestroy(g_stream), g_stream = nullptr;
        g_cuda->releaseContext();
    }
    for (int i = 0; i < 2; i++)
        if (g_tex[i].res) H->texture_remove(M, kTexNames[i]);
    if (g_scene) {
        ReleaseFluid();
        ReleaseCloth();
        ReleaseJelly();
        for (auto &kv : g_movers) kv.second.body->release();
        g_movers.clear();
        Release(g_ground);
        Release(g_scene);
    }
    Release(g_clothMat);
    Release(g_jellyMat);
    Release(g_material);
    Release(g_dispatcher);
    Release(g_physics);
    Release(g_cuda);
    Release(g_foundation);
    if (g_gpuDll) FreeLibrary(g_gpuDll), g_gpuDll = nullptr;
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
    g_opsCh = H->channel_open(mod, "ops", sizeof(gpux::Ops));
    g_frameCh = H->channel_open(mod, "frame", sizeof(gpux::Frame));
    g_outCh = H->channel_open(mod, "out", sizeof(gpux::Out));
    g_topoCh = H->channel_open(mod, "topology", sizeof(gpux::Topology));
    if (!g_opsCh || !g_frameCh || !g_outCh || !g_topoCh) return 1;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_ready = StartPhysX();   // (no GPU: the half stays loaded and says why: `side`, the log)
    if (g_ready) H->log(mod, 1, "PhysX %d.%d.%d on the GPU: %s", PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX, g_gpuName.c_str());
    g_stop = false;
    g_broken = false;
    g_worker = std::thread(Worker);
    H->subscribe(mod, "FRAME", OnFrame, nullptr);
    H->command(mod, "side", "{} -> what the 64-bit half has done: cuda, gpu, steps, step_ms, copy_ms, fluid, cloth, jelly, error", Side, nullptr);
    H->command(mod, "fault", "{} -> a fault on purpose in the 64-bit half (it is unloaded; mod.reload brings it back)", Fault, nullptr);
    H->command(mod, "points", "{} -> where the water's particles are (read back from the GPU after a step): the last measure, and the next asked", Points, nullptr);
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) { Shutdown(); }
