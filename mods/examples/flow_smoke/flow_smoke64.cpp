// flow_smoke's 64-bit half (sdk/include/mml/side.h; the 32-bit half: flow_smoke.cpp; the records: flow_smoke.h;
// docs/MODDING.md "64-bit halves"). What a 32-bit game cannot have: NVIDIA Flow 2 (smoke and fire as gas on a sparse
// grid of voxels, its velocity made divergence-free, vortices kept by vorticity confinement, fuel burnt into heat and
// smoke; ray-marched with self-shadowing), on the GPU through Vulkan, in the game's frame.
//
// Flow is nvflow.dll and nvflowext.dll beside this DLL (CMake mml_flow): loaded by their full paths (wide ones: the
// game may be in C:\Игры\Mafia) before Flow's own loader asks for them by name. Its Vulkan device is made on the
// adapter the game draws on (the sidecar's adapter_luid; Flow's enumerateDevices gives each device's LUID), with
// external memory: CUDA, on the same GPU, is the bridge between Flow's buffers and the shared Direct3D 12 textures the
// game's Direct3D 11 device reads (CUDA imports both: an opaque Win32 handle of Vulkan's memory, a Direct3D 12
// resource). Nothing goes through the CPU.
//
// The worker (a thread of this half's) does two things a frame:
//   at "frame" (the 32-bit half's FRAME): the gas stepped (Flow's simulate and offscreen: the shadows, the colour
//     maps), submitted without waiting: the GPU does it while the game draws;
//   at "view" (the 32-bit half's graphics pass): "color_in" and "depth_in" copied into Flow's buffers (CUDA), Flow's
//     ray march over them from that camera, the result copied into "color_out", and "done" written.
// Flow's callbacks and the worker never call the host but for the channels: errors are kept, logged on the mod thread.
//   call flow_smoke.side  -> {"ready", "gpu", "steps", "renders", "simulate_ms", "render_ms", "gpu_ms", "blocks",
//                             "textures", "error"}
//   call flow_smoke.fault -> a fault on purpose on the sidecar's mod thread (the half is unloaded; mod.reload)
#include <windows.h>
#include <d3d12.h>
#include <cuda.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "NvFlowLoader.h"
#include "json.hpp"
#include "mml/side.h"
#include "flow_smoke.h"

namespace {
const MmlSideHost *H;
MmlSideMod *M;
MmlSideChannel *g_frameCh, *g_viewCh, *g_doneCh, *g_sceneCh, *g_moverCh;
double g_freqMs;

double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_freqMs;
}

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
void FlowLog(NvFlowLogLevel level, const char *format, ...) {
    if (level != eNvFlowLogLevel_error && level != eNvFlowLogLevel_warning) return;
    char b[512];
    va_list a;
    va_start(a, format);
    vsnprintf(b, sizeof b, format, a);
    va_end(a);
    Error(std::string("Flow: ") + b);
}
void FlowLoaderError(const char *s, void *) { Error(std::string("Flow's libraries: ") + (s ? s : "")); }
bool Cu(CUresult r, const char *what) {
    if (r == CUDA_SUCCESS) return true;
    const char *s = nullptr;
    cuGetErrorString(r, &s);
    Error(std::string("CUDA ") + what + ": " + (s ? s : std::to_string(r)));
    return false;
}

// ---- Flow
HMODULE g_dll[2];
NvFlowLoader g_flow;
bool g_flowLoaded;
NvFlowContextInterface g_ci;
NvFlowDeviceManager *g_manager;
NvFlowDevice *g_device;
NvFlowDeviceQueue *g_queue;
NvFlowContext *g_ctx;
NvFlowGrid *g_grid;
std::string g_gpuName;
std::atomic<float> g_gpuMs;
std::atomic<const char *> g_stage{"not started"};   // where Start is (side's "stage")

// ---- CUDA, on the same GPU
CUdevice g_cuDev;
CUcontext g_cuCtx;
CUstream g_stream;

// ---- the state the worker keeps
std::thread g_worker;
std::atomic<bool> g_stop, g_ready, g_broken;
bool g_tried;
fsm::Frame g_rec;   // the last "frame"
uint64_t g_frameSeen, g_viewSeen;
uint32_t g_clearSeen;
std::atomic<uint64_t> g_steps, g_renders;
std::atomic<float> g_simMs, g_renderMs;
std::atomic<uint32_t> g_blocks;

// The shared textures (made on the mod thread at the size the worker asks for) and their CUDA mappings.
const char *const kTexNames[3] = {"color_in", "depth_in", "color_out"};
const uint32_t kTexFormats[3] = {10 /* R16G16B16A16_FLOAT */, 41 /* R32_FLOAT */, 10};
const uint32_t kTexBytes[3] = {8, 4, 8};
struct Tex {
    ID3D12Resource *res = nullptr;
    HANDLE handle = nullptr;
    uint32_t gen = 0;              // the sidecar's generation
    CUexternalMemory ext = nullptr;
    CUmipmappedArray mm = nullptr;
    CUarray arr = nullptr;
    uint32_t mappedGen = 0;
};
std::mutex g_texMx;
Tex g_tex[3];
uint32_t g_texW, g_texH;                 // what was made (g_texMx)
std::atomic<uint32_t> g_wantW, g_wantH;  // what the worker wants

// Flow's buffers at the frame's size, each shared with CUDA (an opaque Win32 handle of its Vulkan memory)
struct Buf {
    NvFlowBuffer *buf = nullptr;
    HANDLE handle = nullptr;
    CUexternalMemory ext = nullptr;
    CUdeviceptr ptr = 0;
    uint64_t bytes = 0;
};
Buf g_buf[3];   // the colour in, the depth in, the colour out
uint32_t g_bufW, g_bufH;

void FreeBuf(Buf &b) {
    if (b.ext) cuDestroyExternalMemory(b.ext);
    if (b.handle) CloseHandle(b.handle);
    if (b.buf) g_ci.destroyBuffer(g_ctx, b.buf);
    b = Buf{};
}

bool MakeBuffers(uint32_t w, uint32_t h) {
    for (Buf &b : g_buf) FreeBuf(b);
    g_bufW = g_bufH = 0;
    for (int i = 0; i < 3; i++) {
        Buf &b = g_buf[i];
        NvFlowBufferDesc d{};
        d.usageFlags = eNvFlowBufferUsage_bufferCopySrc | eNvFlowBufferUsage_bufferCopyDst;
        d.format = eNvFlowFormat_unknown;
        d.structureStride = 0;
        // (a whole number of 64 KiB: Vulkan's allocation is then this size, the size CUDA imports)
        d.sizeInBytes = (uint64_t(w) * h * kTexBytes[i] + 0xFFFF) & ~uint64_t(0xFFFF);
        b.buf = g_ci.createBuffer(g_ctx, eNvFlowMemoryType_device, &d);
        if (!b.buf) { Error("Flow's createBuffer failed"); return false; }
        uint64_t size = 0;
        g_flow.deviceInterface.getBufferExternalHandle(g_ctx, b.buf, &b.handle, sizeof b.handle, &size);
        if (!b.handle) { Error("Flow's buffer has no external handle"); return false; }
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC hd{};
        hd.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
        hd.handle.win32.handle = b.handle;
        hd.size = d.sizeInBytes;
        if (!Cu(cuImportExternalMemory(&b.ext, &hd), "cuImportExternalMemory (Flow's buffer)")) return false;
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bd{};
        bd.size = d.sizeInBytes;
        if (!Cu(cuExternalMemoryGetMappedBuffer(&b.ptr, b.ext, &bd), "cuExternalMemoryGetMappedBuffer")) return false;
        b.bytes = d.sizeInBytes;
    }
    g_bufW = w, g_bufH = h;
    return true;
}

void Unmap(Tex &t) {
    if (t.mm) cuMipmappedArrayDestroy(t.mm);
    if (t.ext) cuDestroyExternalMemory(t.ext);
    t.mm = nullptr, t.ext = nullptr, t.arr = nullptr, t.mappedGen = 0;
}

// The texture's CUDA mapping, made again when the mod thread has made the texture anew (g_texMx held).
bool Map(Tex &t, int i) {
    if (!t.res) return false;
    if (t.mappedGen == t.gen && t.arr) return true;
    Unmap(t);
    auto *dev = static_cast<ID3D12Device *>(H->texture_d3d12_device());
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &rd);
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC hd{};
    hd.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
    hd.handle.win32.handle = t.handle;
    hd.size = ai.SizeInBytes;
    hd.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
    if (!Cu(cuImportExternalMemory(&t.ext, &hd), "cuImportExternalMemory (a shared texture)")) return false;
    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC md{};
    md.arrayDesc.Width = rd.Width;
    md.arrayDesc.Height = rd.Height;
    md.arrayDesc.Format = kTexBytes[i] == 8 ? CU_AD_FORMAT_HALF : CU_AD_FORMAT_FLOAT;
    md.arrayDesc.NumChannels = kTexBytes[i] == 8 ? 4 : 1;
    md.arrayDesc.Flags = CUDA_ARRAY3D_COLOR_ATTACHMENT;   // (a render target's memory)
    md.numLevels = 1;
    if (!Cu(cuExternalMemoryGetMappedMipmappedArray(&t.mm, t.ext, &md), "cuExternalMemoryGetMappedMipmappedArray") ||
        !Cu(cuMipmappedArrayGetLevel(&t.arr, t.mm, 0), "cuMipmappedArrayGetLevel"))
        return false;
    t.mappedGen = t.gen;
    return true;
}

// A texture's pixels into a buffer of rows (in), or back (out).
bool CopyArray(CUarray arr, CUdeviceptr buf, uint32_t w, uint32_t h, uint32_t bpp, bool toBuffer) {
    CUDA_MEMCPY2D c{};
    if (toBuffer) {
        c.srcMemoryType = CU_MEMORYTYPE_ARRAY, c.srcArray = arr;
        c.dstMemoryType = CU_MEMORYTYPE_DEVICE, c.dstDevice = buf, c.dstPitch = size_t(w) * bpp;
    } else {
        c.srcMemoryType = CU_MEMORYTYPE_DEVICE, c.srcDevice = buf, c.srcPitch = size_t(w) * bpp;
        c.dstMemoryType = CU_MEMORYTYPE_ARRAY, c.dstArray = arr;
    }
    c.WidthInBytes = size_t(w) * bpp;
    c.Height = h;
    return Cu(cuMemcpy2DAsync(&c, g_stream), "cuMemcpy2DAsync");
}

// ---- the meshes the gas meets: the scenery ("scenery") and what moves ("movers"), in the arrays Flow's mesh emitter
// reads; a version a change (Flow uploads an array again only when its version is new)
uint64_t g_meshVersion;
struct MeshData {
    uint64_t id = 0;
    uint32_t kind = 0, topology = 0;
    std::vector<NvFlowFloat3> pos, vel;
    std::vector<int> index, counts;   // (Flow's faces: 3 indices a face, a count of 3 each)
    uint64_t posVersion = 0, indexVersion = 0;
    uint64_t Triangles() const { return counts.size(); }
};
MeshData g_scene;
std::vector<MeshData> g_movers;
std::vector<uint8_t> g_sceneIn(fsm::kSceneryBytes), g_moverIn(fsm::kMoversBytes);
uint64_t g_sceneSeen, g_moversSeen;
std::atomic<uint32_t> g_sceneTris, g_moverCount, g_moverTris;

void ReadScenery() {
    uint64_t v = 0;
    uint32_t n = H->channel_read(g_sceneCh, g_sceneIn.data(), uint32_t(g_sceneIn.size()), &v);
    if (n < sizeof(fsm::Scenery) || v == g_sceneSeen) return;
    g_sceneSeen = v;
    const auto *h = reinterpret_cast<const fsm::Scenery *>(g_sceneIn.data());
    if (h->vertices > fsm::kMaxSceneVertices || h->triangles > fsm::kMaxSceneTriangles ||
        n < sizeof *h + 12ull * h->vertices + 12ull * h->triangles)
        return;
    const float *pos = reinterpret_cast<const float *>(h + 1);
    const uint32_t *idx = reinterpret_cast<const uint32_t *>(pos + 3 * h->vertices);
    g_scene.pos.resize(h->vertices);
    memcpy(g_scene.pos.data(), pos, 12ull * h->vertices);
    g_scene.index.resize(3ull * h->triangles);
    for (size_t i = 0; i < g_scene.index.size(); i++) g_scene.index[i] = int(std::min(idx[i], h->vertices ? h->vertices - 1 : 0));
    g_scene.counts.assign(h->triangles, 3);
    g_scene.posVersion = g_scene.indexVersion = ++g_meshVersion;
    g_sceneTris = h->triangles;
}

void ReadMovers() {
    uint64_t v = 0;
    uint32_t n = H->channel_read(g_moverCh, g_moverIn.data(), uint32_t(g_moverIn.size()), &v);
    if (n < sizeof(fsm::Movers) || v == g_moversSeen) return;
    g_moversSeen = v;
    const auto *h = reinterpret_cast<const fsm::Movers *>(g_moverIn.data());
    if (h->count > fsm::kMaxMovers || h->vertices > fsm::kMaxMoverVertices || h->triangles > fsm::kMaxMoverTriangles ||
        n < sizeof *h + 24ull * h->vertices + 12ull * h->triangles)
        return;
    const float *pos = reinterpret_cast<const float *>(h + 1), *vel = pos + 3 * h->vertices;
    const uint32_t *idx = reinterpret_cast<const uint32_t *>(vel + 3 * h->vertices);
    std::vector<MeshData> next(h->count);
    uint32_t tris = 0;
    for (uint32_t i = 0; i < h->count; i++) {
        const fsm::Mover &m = h->mover[i];
        if (m.firstVertex + m.vertices > h->vertices || m.firstTriangle + m.triangles > h->triangles) continue;
        MeshData &d = next[i];
        // (the same actor as before: its arrays kept, only what changed made new)
        auto old = std::find_if(g_movers.begin(), g_movers.end(), [&](const MeshData &o) { return o.id == m.id; });
        if (old != g_movers.end()) d = std::move(*old);
        d.id = m.id, d.kind = m.kind;
        d.pos.resize(m.vertices);
        d.vel.resize(m.vertices);
        memcpy(d.pos.data(), pos + 3 * m.firstVertex, 12ull * m.vertices);
        memcpy(d.vel.data(), vel + 3 * m.firstVertex, 12ull * m.vertices);
        d.posVersion = ++g_meshVersion;
        if (d.topology != m.topology || d.counts.size() != m.triangles) {
            d.topology = m.topology;
            d.index.resize(3ull * m.triangles);
            for (size_t k = 0; k < d.index.size(); k++) d.index[k] = int(std::min(idx[3ull * m.firstTriangle + k], m.vertices ? m.vertices - 1 : 0));
            d.counts.assign(m.triangles, 3);
            d.indexVersion = ++g_meshVersion;
        }
        tris += m.triangles;
    }
    g_movers = std::move(next);
    g_moverCount = h->count, g_moverTris = tris;
}


// ---- the parameters of a frame: Flow's typed snapshots of every layer and emitter
// Layer 0: smoke and steam (a grey colour map lit by the scene); layer 1: fire (Flow's fire colour map: fuel burnt
// into heat, light and smoke), on a finer grid. The ground (a box) and the meshes are in both layers (allLayers, and
// physics collision: both layers collide).
struct Params {
    NvFlowGridSimulateLayerParams simulate[2];
    NvFlowGridOffscreenLayerParams offscreen[2];
    NvFlowGridRenderLayerParams render[2];
    std::vector<NvFlowGridEmitterSphereParams> spheres;
    std::vector<NvFlowGridEmitterBoxParams> boxes;
    std::vector<NvFlowGridEmitterMeshParams> meshes;
    std::vector<NvFlowUint8 *> simulatePtr, offscreenPtr, renderPtr, spherePtr, boxPtr, meshPtr;
    NvFlowDatabaseTypeSnapshot types[6];
    NvFlowGridParamsDescSnapshot snapshot;
    NvFlowGridParamsDesc desc;
    NvFlowFloat4 smokeColors[6];
    uint64_t version = 0;
} P;

// What `tune` changes (the mod thread writes, the worker reads at the next frame's parameters)
struct Tune {
    float cellSize = 0.1f;          // metres, the smoke's: a block of Flow's (32 x 16 x 16 cells) is 3.2 x 1.6 x 1.6 m
    float fireCell = 0.04f;         // metres, the fire's: its tongues are finer than the smoke's billows
    float wall = 0.15f;             // metres each side of a mesh's surface the gas cannot enter
    float wallCouple = 100.f;       // how hard a mesh holds the gas there to its own velocity (Flow's couple rate)
    float gravity = -2.f;           // m/s2 along y: with buoyancyPerTemp, how fast warm gas rises
    float buoyancy = 10.f;          // the smoke layer's buoyancyPerTemp (fire's: half; its burning heats it)
    float cooling = 1.5f;           // the smoke layer's coolingRate (a second's share of its heat lost)
    float vorticity = 2.f;          // the smoke layer's vorticity confinement (fire's: two thirds)
    float smokeFade = 0.2f;         // how fast smoke thins away
    // (Flow's attenuations are per unit of length: 1 - exp(-attenuation * step) a step. Its defaults, 0.05 and 0.045,
    // are Omniverse's centimetres, 5 and 4.5 in metres: a thin haze; these are a fire's thick smoke)
    float attenuation = 8.f;        // the ray march's: how opaque the gas is
    float colorScale = 1.f;         // the ray march's brightness
    float shadowAttenuation = 12.f; // the self-shadowing's
    float shadowMin = 0.25f;        // the light left in the smoke's own shadow (the sky's, all round)
    float shadowSteps = 48;         // the self-shadowing's steps towards the sun (a step: three quarters of a cell)
    float albedo = 0.6f;            // the smoke's own grey: what of the scene's light it gives back
    bool wireframe = false;         // Flow's blocks drawn as boxes (where the grid is allocated)
};
std::mutex g_tuneMx;
Tune g_tune;
// (tune's names for the numbers above)
const struct {
    const char *name;
    float Tune::*field;
} kTuneFields[] = {{"cell_size", &Tune::cellSize}, {"fire_cell", &Tune::fireCell}, {"wall", &Tune::wall},
                   {"wall_couple", &Tune::wallCouple}, {"gravity", &Tune::gravity}, {"buoyancy", &Tune::buoyancy},
                   {"cooling", &Tune::cooling}, {"vorticity", &Tune::vorticity}, {"smoke_fade", &Tune::smokeFade},
                   {"attenuation", &Tune::attenuation}, {"color_scale", &Tune::colorScale},
                   {"shadow_attenuation", &Tune::shadowAttenuation}, {"shadow_min", &Tune::shadowMin},
                   {"shadow_steps", &Tune::shadowSteps}, {"albedo", &Tune::albedo}};

NvFlowFloat4x4 Identity() {
    NvFlowFloat4x4 m{};
    m.x.x = m.y.y = m.z.z = m.w.w = 1;
    return m;
}

// What the gas cannot enter: a shell of `wall` metres each side of the mesh's surface (the game's meshes are often
// open: a wall is one side of triangles, so the shell does not depend on which side they face), where Flow holds the
// gas at the mesh's own velocity (a vertex's, moving) with nothing in it. In every layer that collides (both).
NvFlowGridEmitterMeshParams Obstacle(uint64_t luid, MeshData &d, const Tune &t) {
    NvFlowGridEmitterMeshParams e = NvFlowEmitterMeshParams_default;
    e.luid = luid;
    e.isPhysicsCollision = NV_FLOW_TRUE;
    e.localToWorld = e.localToWorldVelocity = Identity();
    e.velocityIsWorldSpace = NV_FLOW_TRUE;
    e.minDistance = -t.wall, e.maxDistance = t.wall;
    e.allocateMask = NV_FLOW_FALSE;   // (it acts where the gas is; it makes no blocks of its own)
    e.velocity = NvFlowFloat3{0, 0, 0};
    e.divergence = e.temperature = e.fuel = e.burn = e.smoke = 0;
    e.coupleRateVelocity = e.coupleRateTemperature = e.coupleRateFuel = e.coupleRateBurn = e.coupleRateSmoke = t.wallCouple;
    e.coupleRateDivergence = 0;
    e.meshPositions = d.pos.data(), e.meshPositionCount = d.pos.size(), e.meshPositionVersion = d.posVersion;
    e.meshFaceVertexIndices = d.index.data(), e.meshFaceVertexIndexCount = d.index.size(), e.meshFaceVertexIndexVersion = d.indexVersion;
    e.meshFaceVertexCounts = d.counts.data(), e.meshFaceVertexCountCount = d.counts.size(), e.meshFaceVertexCountVersion = d.indexVersion;
    if (!d.vel.empty()) e.meshVelocities = d.vel.data(), e.meshVelocityCount = d.vel.size(), e.meshVelocityVersion = d.posVersion;
    return e;
}

void BuildParams(const fsm::Frame &f, bool clear) {
    Tune t;
    {
        std::lock_guard<std::mutex> lk(g_tuneMx);
        t = g_tune;
    }
    P.version++;
    for (int layer = 0; layer < 2; layer++) {
        NvFlowGridSimulateLayerParams &s = P.simulate[layer];
        s = NvFlowGridSimulateLayerParams_default;
        s.luid = 1 + layer;
        s.layer = layer;
        s.densityCellSize = layer ? t.fireCell : t.cellSize;
        s.physicsCollisionEnabled = NV_FLOW_TRUE;   // (the meshes' obstacles: every layer)
        s.stepsPerSecond = 60;
        s.maxStepsPerSimulate = 4;
        s.enableVariableTimeStep = NV_FLOW_FALSE;
        s.forceClear = clear;
        s.simulateWhenPaused = NV_FLOW_FALSE;
        s.blockMinLifetime = 8;
        // metres, y up (Flow's defaults are Omniverse's: centimetres, z up)
        s.advection.gravity = NvFlowFloat3{0, t.gravity, 0};
        s.advection.buoyancyPerTemp = layer ? t.buoyancy / 2 : t.buoyancy;
        if (!layer) s.advection.coolingRate = t.cooling;
        s.vorticity.forceScale = layer ? t.vorticity * 2 / 3 : t.vorticity;
        // (combustion stays on in the smoke layer too: Flow's buoyancy and cooling are part of it; with no fuel
        // there nothing burns)
        if (!layer) {
            s.advection.smoke.fade = t.smokeFade;   // (slower than the default: a column that lingers)
            s.advection.smoke.damping = 0.1f;
        }
        NvFlowGridOffscreenLayerParams &o = P.offscreen[layer];
        o = NvFlowGridOffscreenLayerParams_default;
        o.luid = 11 + layer;
        o.layer = layer;
        o.shadow.lightDirection = NvFlowFloat3{f.toSun[0], f.toSun[1], f.toSun[2]};
        o.shadow.minIntensity = t.shadowMin;
        o.shadow.attenuation = t.shadowAttenuation;
        o.shadow.numSteps = NvFlowUint(std::clamp(t.shadowSteps, 1.f, 256.f));
        if (layer) {
            o.colormap.rgbaPoints = NvFlowRayMarchColormapParams_default_rgbaPoints_fire;
        } else {
            // (Flow's colour map is looked up by temperature, its alpha times the smoke's density. Its own smoke map
            // is clear at temperature 0: smoke that has cooled would be gone. Here the grey is the same at every
            // temperature: the smoke shows as long as it is there, warm or not)
            for (int i = 0; i < 6; i++)
                P.smokeColors[i] = NvFlowFloat4{t.albedo * f.light[0], t.albedo * f.light[1], t.albedo * f.light[2], 0.9f};
            o.colormap.rgbaPoints = P.smokeColors;
            o.colormap.colorScale = 1;   // (Flow's 2.5 is for Omniverse's HDR: here the grey is the scene's light)
        }
        NvFlowGridRenderLayerParams &r = P.render[layer];
        r = NvFlowGridRenderLayerParams_default;
        r.luid = 21 + layer;
        r.layer = layer;
        r.rayMarch.attenuation = t.attenuation;
        r.rayMarch.colorScale = t.colorScale;
        r.rayMarch.enableBlockWireframe = t.wireframe;
    }
    P.spheres.clear();
    P.boxes.clear();
    // the sources
    for (uint32_t i = 0; i < f.sources; i++) {
        const fsm::Source &src = f.source[i];
        NvFlowGridEmitterSphereParams e = NvFlowEmitterSphereParams_default;
        e.luid = 1000 + src.id;
        e.layer = src.kind == fsm::kFire ? 1 : 0;
        e.position = NvFlowFloat3{src.at[0], src.at[1], src.at[2]};
        e.radius = src.radius;
        e.velocityIsWorldSpace = NV_FLOW_TRUE;
        e.velocity = NvFlowFloat3{src.velocity[0] + f.wind[0], src.velocity[1] + f.wind[1], src.velocity[2] + f.wind[2]};
        float k = src.strength;
        switch (src.kind) {
        case fsm::kFire:
            e.temperature = 2.f, e.fuel = 1.5f * k, e.smoke = 0;
            e.coupleRateVelocity = 6, e.coupleRateTemperature = 6, e.coupleRateFuel = 6 * k, e.coupleRateSmoke = 0;
            break;
        case fsm::kSteam:
            e.temperature = 0.6f, e.fuel = 0, e.smoke = 0.6f * k;
            e.coupleRateVelocity = 8, e.coupleRateTemperature = 6, e.coupleRateFuel = 0, e.coupleRateSmoke = 6 * k;
            break;
        default:
            e.temperature = 1.f, e.fuel = 0, e.smoke = 1.f * k;
            e.coupleRateVelocity = 4, e.coupleRateTemperature = 4, e.coupleRateFuel = 0, e.coupleRateSmoke = 4 * k;
            break;
        }
        P.spheres.push_back(e);
    }
    // the meshes the game draws: the scenery holds the gas off, each car and person carries it at its vertices' speed
    P.meshes.clear();
    if (g_scene.Triangles()) P.meshes.push_back(Obstacle(4001, g_scene, t));
    for (MeshData &m : g_movers)
        if (m.Triangles()) P.meshes.push_back(Obstacle(5000 + (m.id & 0xFFFFFF), m, t));
    // the ground: below it the gas is held still and taken away (it spreads along it)
    if (f.sources) {
        float cx = 0, cz = 0;
        for (uint32_t i = 0; i < f.sources; i++) cx += f.source[i].at[0], cz += f.source[i].at[2];
        cx /= f.sources, cz /= f.sources;
        NvFlowGridEmitterBoxParams e = NvFlowEmitterBoxParams_default;
        e.luid = 4000;
        e.allLayers = NV_FLOW_TRUE;
        e.localToWorld = Identity();
        e.velocityIsWorldSpace = NV_FLOW_TRUE;
        e.position = NvFlowFloat3{cx, f.ground - 1.f, cz};
        e.halfSize = NvFlowFloat3{48, 1, 48};
        e.velocity = NvFlowFloat3{0, 0, 0};
        e.temperature = e.fuel = e.burn = e.smoke = 0;
        e.coupleRateVelocity = e.coupleRateTemperature = e.coupleRateFuel = e.coupleRateBurn = e.coupleRateSmoke = 100;
        e.allocationScale = 0;
        P.boxes.push_back(e);
    }
    auto ptrs = [](auto &items, std::vector<NvFlowUint8 *> &out) {
        out.clear();
        for (auto &it : items) out.push_back(reinterpret_cast<NvFlowUint8 *>(&it));
    };
    ptrs(P.simulate, P.simulatePtr);
    ptrs(P.offscreen, P.offscreenPtr);
    ptrs(P.render, P.renderPtr);
    ptrs(P.spheres, P.spherePtr);
    ptrs(P.boxes, P.boxPtr);
    ptrs(P.meshes, P.meshPtr);
    P.types[5] = {P.version, &NvFlowGridEmitterMeshParams_NvFlowReflectDataType, P.meshPtr.data(), P.meshPtr.size()};
    P.types[0] = {P.version, &NvFlowGridSimulateLayerParams_NvFlowReflectDataType, P.simulatePtr.data(), P.simulatePtr.size()};
    P.types[1] = {P.version, &NvFlowGridOffscreenLayerParams_NvFlowReflectDataType, P.offscreenPtr.data(), P.offscreenPtr.size()};
    P.types[2] = {P.version, &NvFlowGridRenderLayerParams_NvFlowReflectDataType, P.renderPtr.data(), P.renderPtr.size()};
    P.types[3] = {P.version, &NvFlowGridEmitterSphereParams_NvFlowReflectDataType, P.spherePtr.data(), P.spherePtr.size()};
    P.types[4] = {P.version, &NvFlowGridEmitterBoxParams_NvFlowReflectDataType, P.boxPtr.data(), P.boxPtr.size()};
    P.snapshot = NvFlowGridParamsDescSnapshot{};
    P.snapshot.snapshot = NvFlowDatabaseSnapshot{P.version, P.types, 6};
    P.snapshot.absoluteSimTime = f.time;
    P.snapshot.deltaTime = f.run ? f.dt : 0;
    P.snapshot.globalForceClear = clear;
    P.desc = NvFlowGridParamsDesc{&P.snapshot, 1};
}

// ---- starting: Flow's libraries, a Vulkan device and CUDA on the game's adapter, a grid
std::wstring Beside(const wchar_t *name) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Beside), &self);
    wchar_t path[MAX_PATH * 4];
    std::wstring dll(path, GetModuleFileNameW(self, path, MAX_PATH * 4));
    return dll.substr(0, dll.find_last_of(L"\\/") + 1) + name;
}

// Flow's profiler: each flush's passes (the step's and the ray march's), their GPU times kept by name as a running
// mean (side's "gpu_top")
std::mutex g_profMx;
std::vector<std::pair<std::string, float>> g_prof;
void NV_FLOW_ABI Profile(void *, NvFlowUint64, NvFlowUint n, NvFlowProfilerEntry *e) {
    float sum = 0;
    std::lock_guard<std::mutex> lk(g_profMx);
    for (NvFlowUint i = 0; i < n; i++) {
        float ms = e[i].gpuDeltaTime * 1000.f;
        sum += ms;
        const char *label = e[i].label ? e[i].label : "?";
        auto it = std::find_if(g_prof.begin(), g_prof.end(), [&](const auto &p) { return p.first == label; });
        if (it == g_prof.end()) g_prof.push_back({label, ms});
        else it->second += 0.05f * (ms - it->second);
    }
    g_gpuMs = sum;
}

bool LoadFlow() {
    // (by their full paths first: Flow's loader asks for them by name with LoadLibraryA, which finds them loaded)
    const wchar_t *names[2] = {L"nvflow.dll", L"nvflowext.dll"};
    for (int i = 0; i < 2; i++) {
        g_dll[i] = LoadLibraryW(Beside(names[i]).c_str());
        if (!g_dll[i]) {
            Error("nvflow.dll or nvflowext.dll was not loaded from beside the half (" + std::to_string(GetLastError()) + ")");
            return false;
        }
    }
    NvFlowLoaderInitCustom(&g_flow, FlowLoaderError, nullptr, "nvflow.dll", "", "nvflowext.dll", "");
    g_flowLoaded = true;
    if (!g_flow.module_nvflow || !g_flow.module_nvflowext || !g_flow.deviceInterface.createDeviceManager) {
        Error("Flow's interfaces were not found");
        return false;
    }
    return true;
}

bool Start(const uint32_t luid[2]) {
    g_stage = "CUDA";
    if (!Cu(cuInit(0), "cuInit")) return false;
    int n = 0;
    cuDeviceGetCount(&n);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        CUdevice d;
        char l[8];
        unsigned mask = 0;
        if (cuDeviceGet(&d, i) == CUDA_SUCCESS && cuDeviceGetLuid(l, &mask, d) == CUDA_SUCCESS && !memcmp(l, luid, 8)) g_cuDev = d, found = true;
    }
    if (!found) { Error("no CUDA device on the game's adapter (Flow's bridge to the game needs an NVIDIA GPU)"); return false; }
    char name[256] = {};
    cuDeviceGetName(name, sizeof name, g_cuDev);
    g_gpuName = name;
    if (!Cu(cuDevicePrimaryCtxRetain(&g_cuCtx, g_cuDev), "cuDevicePrimaryCtxRetain") || !Cu(cuCtxSetCurrent(g_cuCtx), "cuCtxSetCurrent") ||
        !Cu(cuStreamCreate(&g_stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate"))
        return false;
    g_stage = "Flow's device manager";
    g_manager = g_flow.deviceInterface.createDeviceManager(NV_FLOW_FALSE, nullptr, 0);
    if (!g_manager) { Error("Flow's device manager (Vulkan) was not made"); return false; }
    NvFlowUint index = ~0u;
    NvFlowPhysicalDeviceDesc pd{};
    for (NvFlowUint i = 0; g_flow.deviceInterface.enumerateDevices(g_manager, i, &pd); i++)
        if (pd.deviceLUIDValid && !memcmp(pd.deviceLUID, luid, 8)) { index = i; break; }
    if (index == ~0u) { Error("no Vulkan device of Flow's on the game's adapter"); return false; }
    g_stage = "Flow's Vulkan device";
    NvFlowDeviceDesc dd{};
    dd.deviceIndex = index;
    dd.enableExternalUsage = NV_FLOW_TRUE;
    dd.logPrint = FlowLog;
    g_device = g_flow.deviceInterface.createDevice(g_manager, &dd);
    if (!g_device) { Error("Flow's Vulkan device was not made"); return false; }
    g_queue = g_flow.deviceInterface.getDeviceQueue(g_device);
    NvFlowContextInterface_duplicate(&g_ci, g_flow.deviceInterface.getContextInterface(g_queue));
    g_ctx = g_flow.deviceInterface.getContext(g_queue);
    if (!g_ci.isFeatureSupported(g_ctx, eNvFlowContextFeature_bufferExternalHandle)) {
        Error("Flow's device cannot share its buffers (external memory)");
        return false;
    }
    g_stage = "Flow's grid";
    NvFlowGridDesc gd = NvFlowGridDesc_default;
    g_grid = g_flow.gridInterface.createGrid(&g_ci, g_ctx, g_flow.opList_orig, g_flow.extOpList_orig, &gd);
    if (!g_grid) { Error("Flow's grid was not made"); return false; }
    g_flow.deviceInterface.enableProfiler(g_ctx, nullptr, Profile);
    g_stage = "ready";
    return true;
}

// ---- a frame's work
void Simulate() {
    bool clear = g_rec.clear != g_clearSeen;
    g_clearSeen = g_rec.clear;
    double t0 = Now();
    BuildParams(g_rec, clear);
    g_flow.gridInterface.simulate(g_ctx, g_grid, &P.desc, clear);
    g_flow.gridInterface.offscreen(g_ctx, g_grid, &P.desc);
    NvFlowUint64 id = 0;
    if (g_flow.deviceInterface.flush(g_queue, &id, nullptr, nullptr)) {
        Error("Flow's device was lost");
        g_broken = true;
    }
    g_blocks = g_flow.gridInterface.getActiveBlockCount(g_grid);
    g_simMs = float(Now() - t0);
    g_steps++;
}

void Answer(const fsm::View &v, bool ok, uint32_t outGen) {
    fsm::Done d{};
    d.frame = v.frame;
    d.askQpc = v.askQpc;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    d.doneQpc = t.QuadPart;
    d.ok = ok;
    d.blocks = g_blocks;
    d.outGeneration = outGen;
    d.steps = uint32_t(g_steps);
    d.simulateMs = g_simMs;
    d.renderMs = g_renderMs;
    d.gpuMs = g_gpuMs;
    d.sceneTriangles = g_sceneTris;
    snprintf(d.error, sizeof d.error, "%s", LastError().c_str());
    H->channel_write(g_doneCh, &d, sizeof d);
}

NvFlowFloat4x4 Matrix(const float *m) {
    NvFlowFloat4x4 r;
    memcpy(&r, m, sizeof r);
    return r;
}

void Render(const fsm::View &v) {
    const uint32_t w = v.width, h = v.height;
    if (!w || !h || w > 8192 || h > 8192) return Answer(v, false, 0);
    // the textures at this size: asked of the mod thread (it makes them), this frame goes without
    std::unique_lock<std::mutex> lk(g_texMx);
    if (g_texW != w || g_texH != h || !g_tex[0].res || !g_tex[1].res || !g_tex[2].res) {
        g_wantW = w, g_wantH = h;
        lk.unlock();
        return Answer(v, false, 0);
    }
    // (the 32-bit half drew into the generation it names: one made since is not written yet)
    if (v.inGeneration[0] != g_tex[0].gen || v.inGeneration[1] != g_tex[1].gen) {
        uint32_t gen = g_tex[2].gen;
        lk.unlock();
        return Answer(v, false, gen);
    }
    bool ok = Map(g_tex[0], 0) && Map(g_tex[1], 1) && Map(g_tex[2], 2);
    CUarray in = g_tex[0].arr, depth = g_tex[1].arr, out = g_tex[2].arr;
    uint32_t outGen = g_tex[2].gen;
    lk.unlock();
    if (!ok) return Answer(v, false, 0);
    if ((g_bufW != w || g_bufH != h) && !MakeBuffers(w, h)) return Answer(v, false, 0);
    double t0 = Now();
    // the frame into Flow's buffers
    ok = CopyArray(in, g_buf[0].ptr, w, h, 8, true) && CopyArray(depth, g_buf[1].ptr, w, h, 4, true) &&
         Cu(cuStreamSynchronize(g_stream), "cuStreamSynchronize (in)");
    if (!ok) return Answer(v, false, 0);
    // Flow: the buffers into textures, the ray march over them, the result into the out buffer
    NvFlowBufferTransient *bIn = g_ci.registerBufferAsTransient(g_ctx, g_buf[0].buf);
    NvFlowBufferTransient *bDepth = g_ci.registerBufferAsTransient(g_ctx, g_buf[1].buf);
    NvFlowBufferTransient *bOut = g_ci.registerBufferAsTransient(g_ctx, g_buf[2].buf);
    NvFlowTextureDesc td{};
    td.textureType = eNvFlowTextureType_2d;
    td.usageFlags = eNvFlowTextureUsage_texture | eNvFlowTextureUsage_textureCopyDst;
    td.format = eNvFlowFormat_r16g16b16a16_float;
    td.width = w, td.height = h, td.depth = 1, td.mipLevels = 1;
    NvFlowTextureTransient *tIn = g_ci.getTextureTransient(g_ctx, &td);
    td.format = eNvFlowFormat_r32_float;
    NvFlowTextureTransient *tDepth = g_ci.getTextureTransient(g_ctx, &td);
    NvFlowPassCopyBufferToTextureParams cp{};
    cp.bufferRowPitch = w * 8, cp.bufferDepthPitch = w * 8 * h;
    cp.textureExtent = NvFlowUint3{w, h, 1};
    cp.src = bIn, cp.dst = tIn, cp.debugLabel = "flow_smoke color in";
    g_ci.addPassCopyBufferToTexture(g_ctx, &cp);
    cp.bufferRowPitch = w * 4, cp.bufferDepthPitch = w * 4 * h;
    cp.src = bDepth, cp.dst = tDepth, cp.debugLabel = "flow_smoke depth in";
    g_ci.addPassCopyBufferToTexture(g_ctx, &cp);
    NvFlowFloat4x4 view = Matrix(v.view), proj = Matrix(v.proj);
    NvFlowTextureTransient *tOut = nullptr;
    g_flow.gridInterface.render(g_ctx, g_grid, &P.desc, &view, &proj, &proj, w, h, w, h, 1.f, tDepth,
                                eNvFlowFormat_r16g16b16a16_float, tIn, &tOut);
    if (!tOut) tOut = tIn;
    NvFlowPassCopyTextureToBufferParams co{};
    co.bufferRowPitch = w * 8, co.bufferDepthPitch = w * 8 * h;
    co.textureExtent = NvFlowUint3{w, h, 1};
    co.src = tOut, co.dst = bOut, co.debugLabel = "flow_smoke color out";
    g_ci.addPassCopyTextureToBuffer(g_ctx, &co);
    NvFlowUint64 id = 0;
    if (g_flow.deviceInterface.flush(g_queue, &id, nullptr, nullptr)) {
        Error("Flow's device was lost");
        g_broken = true;
        return Answer(v, false, 0);
    }
    g_flow.deviceInterface.waitForFrame(g_queue, id);
    // the result into "color_out"
    ok = CopyArray(out, g_buf[2].ptr, w, h, 8, false) && Cu(cuStreamSynchronize(g_stream), "cuStreamSynchronize (out)");
    g_renderMs = float(Now() - t0);
    g_renders++;
    Answer(v, ok, outGen);
}

void Work() {
    if (!g_ready) {
        uint32_t luid[2] = {};
        if (g_tried || !H->adapter_luid(luid)) return;   // (the game's adapter is known once its graphics bridge runs)
        g_tried = true;
        g_ready = Start(luid);
        if (!g_ready) return;
    }
    uint64_t fv = 0;
    if (H->channel_read(g_frameCh, &g_rec, sizeof g_rec, &fv) >= offsetof(fsm::Frame, source) && fv != g_frameSeen) {
        g_frameSeen = fv;
        ReadScenery();
        ReadMovers();
        if (g_rec.run || g_rec.clear != g_clearSeen) Simulate();
    }
    fsm::View v;
    uint64_t vv = 0;
    if (P.version && H->channel_read(g_viewCh, &v, sizeof v, &vv) == sizeof v && vv != g_viewSeen) {
        g_viewSeen = vv;
        Render(v);
    }
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
        H->channel_wait(g_viewCh, g_viewSeen, 1);   // (a pass's ask wakes it at once; "frame" is looked at each millisecond)
        if (g_stop) break;
        if (g_broken) { Sleep(10); continue; }
        if (g_cuCtx) cuCtxSetCurrent(g_cuCtx);
        if (int code = GuardedWork()) {
            g_broken = true;
            char b[128];
            snprintf(b, sizeof b, "a fault (0x%08X) in Flow's work: stopped (mod.reload starts again)", static_cast<unsigned>(code));
            Error(b);
        }
    }
}

// ---- the mod thread: the textures at the size the worker wants, the errors logged
void MakeTextures() {
    uint32_t w = g_wantW, h = g_wantH;
    if (!w || !h || !H->texture_d3d12_device()) return;
    {
        std::lock_guard<std::mutex> lk(g_texMx);
        if (w == g_texW && h == g_texH) return;
        for (Tex &t : g_tex) Unmap(t);
    }
    for (int i = 0; i < 3; i++) {
        MmlSideTexture t{};
        t.size = sizeof t;
        if (!H->texture_create(M, kTexNames[i], w, h, kTexFormats[i], &t)) {
            Error(std::string("texture_create ") + kTexNames[i] + " failed");
            return;
        }
        std::lock_guard<std::mutex> lk(g_texMx);
        g_tex[i].res = static_cast<ID3D12Resource *>(t.d3d12_resource);
        g_tex[i].handle = t.shared_handle;
        g_tex[i].gen = t.generation;
    }
    std::lock_guard<std::mutex> lk(g_texMx);
    g_texW = w, g_texH = h;
}

void OnFrame(MmlSideMod *, const char *, const char *, void *) {
    if (g_ready && (g_wantW != g_texW || g_wantH != g_texH)) MakeTextures();
    std::string log;
    {
        std::lock_guard<std::mutex> lk(g_errMx);
        log.swap(g_toLog);
    }
    if (!log.empty()) H->log(M, 2, "%s", log.c_str());
}

int Side(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    std::string e = LastError();
    for (char &c : e)
        if (c == '"' || c == '\\' || c == '\n') c = '\'';
    char b[1024];
    snprintf(b, sizeof b,
             "{\"ready\": %s, \"stage\": \"%s\", \"broken\": %s, \"gpu\": \"%s\", \"steps\": %llu, \"renders\": %llu, \"simulate_ms\": %.3f, \"render_ms\": %.3f, "
             "\"gpu_ms\": %.3f, \"blocks\": %u, \"textures\": [%u, %u], \"buffers\": [%u, %u], \"scene_triangles\": %u, "
             "\"movers\": %u, \"mover_triangles\": %u, \"error\": \"%s\"}",
             g_ready ? "true" : "false", g_stage.load(), g_broken ? "true" : "false", g_gpuName.c_str(), static_cast<unsigned long long>(g_steps.load()),
             static_cast<unsigned long long>(g_renders.load()), g_simMs.load(), g_renderMs.load(), g_gpuMs.load(), g_blocks.load(), g_texW, g_texH,
             g_bufW, g_bufH, g_sceneTris.load(), g_moverCount.load(), g_moverTris.load(), e.c_str());
    // (Flow's dearest passes, ms of GPU a frame)
    std::vector<std::pair<std::string, float>> prof;
    {
        std::lock_guard<std::mutex> lk(g_profMx);
        prof = g_prof;
    }
    std::sort(prof.begin(), prof.end(), [](const auto &x, const auto &y) { return x.second > y.second; });
    nlohmann::json top = nlohmann::json::object();
    for (size_t i = 0; i < prof.size() && i < 10; i++) top[prof[i].first] = std::round(prof[i].second * 1000) / 1000;
    std::string out(b);
    out.pop_back();
    out += ", \"gpu_top\": " + top.dump() + "}";
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}

int TuneCmd(MmlSideMod *, const char *args, MmlSideReply *r, void *) {
    nlohmann::json a = nlohmann::json::parse(args ? args : "{}", nullptr, false);
    if (!a.is_object()) a = nlohmann::json::object();
    Tune t;
    {
        std::lock_guard<std::mutex> lk(g_tuneMx);
        for (const auto &f : kTuneFields)
            if (a.contains(f.name) && a[f.name].is_number()) g_tune.*f.field = a[f.name].get<float>();
        if (a.contains("wireframe") && a["wireframe"].is_boolean()) g_tune.wireframe = a["wireframe"].get<bool>();
        g_tune.cellSize = std::clamp(g_tune.cellSize, 0.02f, 2.f);
        t = g_tune;
    }
    nlohmann::json out = {{"wireframe", t.wireframe}};
    for (const auto &f : kTuneFields) out[f.name] = t.*f.field;
    std::string s = out.dump();
    H->reply_append(r, s.c_str(), s.size());
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
        g_worker.join();
    }
    if (g_queue) g_flow.deviceInterface.waitIdle(g_queue);
    if (g_cuCtx) cuCtxSetCurrent(g_cuCtx);
    for (Buf &b : g_buf) FreeBuf(b);
    for (Tex &t : g_tex) Unmap(t);
    if (g_stream) cuStreamDestroy(g_stream), g_stream = nullptr;
    if (g_cuCtx) cuDevicePrimaryCtxRelease(g_cuDev), g_cuCtx = nullptr;
    for (int i = 0; i < 3; i++)
        if (g_tex[i].res) H->texture_remove(M, kTexNames[i]);
    if (g_grid) {
        g_flow.deviceInterface.disableProfiler(g_ctx);
        g_flow.gridInterface.destroyGrid(g_ctx, g_grid);
        NvFlowUint64 id = 0;
        g_flow.deviceInterface.flush(g_queue, &id, nullptr, nullptr);
        g_flow.deviceInterface.waitIdle(g_queue);
        g_grid = nullptr;
    }
    if (g_device) g_flow.deviceInterface.destroyDevice(g_manager, g_device), g_device = nullptr;
    if (g_manager) g_flow.deviceInterface.destroyDeviceManager(g_manager), g_manager = nullptr;
    if (g_flowLoaded) NvFlowLoaderDestroy(&g_flow), g_flowLoaded = false;   // (Flow's own references)
    for (HMODULE &d : g_dll)
        if (d) FreeLibrary(d), d = nullptr;
}
} // namespace

MML_EXPORT uint32_t mml_side_api(void) { return MML_SIDE_API_VERSION; }

MML_EXPORT int mml_side_load(const MmlSideHost *host, MmlSideMod *mod) {
    H = host;
    M = mod;
    if (!MML_SIDE_HAS(host, channel_wait)) return 1;   // (a sidecar older than this half)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_frameCh = H->channel_open(mod, "frame", sizeof(fsm::Frame));
    g_viewCh = H->channel_open(mod, "view", sizeof(fsm::View));
    g_doneCh = H->channel_open(mod, "done", sizeof(fsm::Done));
    g_sceneCh = H->channel_open(mod, "scenery", fsm::kSceneryBytes);
    g_moverCh = H->channel_open(mod, "movers", fsm::kMoversBytes);
    if (!g_frameCh || !g_viewCh || !g_doneCh || !g_sceneCh || !g_moverCh) return 1;
    if (!LoadFlow()) H->log(mod, 2, "%s", LastError().c_str());   // (the half stays loaded and says why: `side`)
    else H->log(mod, 1, "NVIDIA Flow loaded; its Vulkan device is made on the game's adapter once the game draws");
    g_stop = false;
    g_broken = !g_flowLoaded;
    g_worker = std::thread(Worker);
    H->subscribe(mod, "FRAME", OnFrame, nullptr);
    H->command(mod, "side", "{} -> what the 64-bit half has done: ready, gpu, steps, renders, simulate_ms, render_ms, gpu_ms, blocks, error", Side, nullptr);
    H->command(mod, "tune", "{cell_size, gravity, buoyancy, cooling, vorticity, smoke_fade, attenuation, color_scale, shadow_attenuation, shadow_min, shadow_steps, albedo, wireframe} -> Flow's parameters now", TuneCmd, nullptr);
    H->command(mod, "fault","{} -> a fault on purpose in the 64-bit half (it is unloaded; mod.reload brings it back)", Fault, nullptr);
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) { Shutdown(); }
