// boxes: a heavy mod's example. A third-party library (Jolt Physics, pinned by tools/deps.py and built with the
// loader for x86), shaders compiled in the mod's build (boxes.hlsl, mml_add_mod's SHADERS), a Direct3D 11 device and a
// thread pool of its own; everything it made is given back at the unload, so it hot-reloads like a small mod.
//
// What it does: a pile of boxes falls onto a floor and a plank in a Jolt world of the mod's own (not the game's:
// nothing of the city is touched); once they all sleep they are dropped again. A Direct3D 11 device draws them every
// frame (with the floor and the plank) with the mod's shaders into a texture of its own and counts on the GPU the
// pixels drawn (read back a few frames later, never waiting); the overlay draws the pile from the side with the game's 2D lines and tells the
// device's state. The world goes on across a reload (its bodies kept with state_put); the device is made anew each
// load: `mod.reload {"id": "boxes", "times": 20}` measures what a reload costs. It needs no game: the offline tests run
// it in the stand-in game.
//
// Where each part is:
//   the library   WorldInit, Drop, WorldStep, WorldFree   Jolt: its allocator and factory, the job threads, the bodies
//   the shaders   boxes.hlsl, boxes.hlsli -> shaders/VsBox.h, PsBox.h, CsCoverage.h (made by the build), GpuCreate
//   the device    GpuCreate, GpuFrame (its loss found and the device made again), GpuRelease (every reference back)
//   the reload    SaveState / RestoreState: the bodies and the counts across loads (state_put, state_get)
//   the unload    mml_mod_unload: the state kept, the device released, the job threads joined, Jolt's globals freed
//   commands      boxes.status, boxes.drop, boxes.snapshot (the GPU's picture as a BMP in the data folder)
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "mml/path.h"

#include "shaders/VsBox.h"        // the build's: const BYTE kVsBox[] (fxc /Fh)
#include "shaders/PsBox.h"
#include "shaders/CsCoverage.h"

namespace {
const MmlHost *H;
MmlMod *M;

// ---- settings (config.json)
struct Settings {
    int boxes = 48, threads = 2, gpuSize = 256;
    bool gpu = true, warp = false, show = true;
    float x = 20, y = 150, scale = 1;
} S;

// ---- what survives a reload (state_put at the unload, state_get at the load)
struct History {
    uint32_t loads, devicesMade, devicesLost, drops;
    uint64_t gpuFrames;
    int32_t lastRefsLeft;      // the device's references left at the last unload (0: all released)
} Hist;
constexpr uint32_t kStateVersion = 1;
struct SavedBody { float pos[3], rot[4], lin[3], ang[3], half[3]; uint32_t awake; };

// ---- the Jolt world
namespace Layers { constexpr JPH::ObjectLayer kStatic = 0, kMoving = 1, kCount = 2; }

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(l)); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override { return l.GetValue() ? "moving" : "static"; }
#endif
};
class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override { return a == Layers::kMoving || b.GetValue() == Layers::kMoving; }
};
class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == Layers::kMoving || b == Layers::kMoving; }
};

struct World {
    BroadPhaseLayers bp;
    ObjectVsBroadPhase ovb;
    ObjectPairs pairs;
    JPH::TempAllocatorImpl *temp = nullptr;
    JPH::JobSystemThreadPool *jobs = nullptr;
    JPH::PhysicsSystem *phys = nullptr;
    std::vector<JPH::BodyID> statics, bodies;
    std::vector<JPH::Vec3> half;        // each box's half extents
    double simTime = 0, sinceDrop = 0, asleepFor = 0, acc = 0;
    uint64_t steps = 0;
    float stepMs = 0;                   // a step's time on the game thread (mean)
    int awake = 0;
    bool restored = false;              // this load went on from the last one's bodies
} *W;

constexpr float kStep = 1.0f / 60.0f;

void JoltTrace(const char *fmt, ...) {   // (Jolt may call it from its job threads: the log is thread-safe)
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    H->log(M, MML_LOG_INFO, "jolt: %s", line);
}
#ifdef JPH_ENABLE_ASSERTS
bool JoltAssert(const char *expr, const char *msg, const char *file, JPH::uint line) {
    H->log(M, MML_LOG_ERROR, "jolt assert %s:%u: %s %s", file, line, expr, msg ? msg : "");
    return false;   // (true would break into a debugger)
}
#endif

uint32_t g_seed;
float Rand(float lo, float hi) { g_seed = g_seed * 1664525u + 1013904223u; return lo + (hi - lo) * ((g_seed >> 8) / 16777216.0f); }

void RemoveBoxes() {
    JPH::BodyInterface &bi = W->phys->GetBodyInterface();
    if (!W->bodies.empty()) {
        bi.RemoveBodies(W->bodies.data(), static_cast<int>(W->bodies.size()));
        bi.DestroyBodies(W->bodies.data(), static_cast<int>(W->bodies.size()));
    }
    W->bodies.clear();
    W->half.clear();
}

void AddBox(JPH::Vec3 half, JPH::RVec3 pos, JPH::Quat rot, JPH::Vec3 lin, JPH::Vec3 ang, bool awake) {
    JPH::BodyInterface &bi = W->phys->GetBodyInterface();
    JPH::BodyCreationSettings bs(new JPH::BoxShape(half), pos, rot, JPH::EMotionType::Dynamic, Layers::kMoving);
    bs.mLinearVelocity = lin;
    bs.mAngularVelocity = ang;
    bs.mFriction = 0.6f;
    JPH::BodyID id = bi.CreateAndAddBody(bs, awake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return;   // (the world is full: maxBodies)
    W->bodies.push_back(id);
    W->half.push_back(half);
}

// A new pile above the plank's top: a column of layers of 3 x 3, each box of its own size and turn.
void Drop() {
    RemoveBoxes();
    g_seed = 12345u + Hist.drops * 7919u;
    for (int i = 0; i < S.boxes; i++) {
        int layer = i / 9, cell = i % 9;
        JPH::Vec3 half(Rand(0.15f, 0.35f), Rand(0.15f, 0.35f), Rand(0.15f, 0.35f));
        JPH::RVec3 pos(1.2f + (cell % 3 - 1) * 0.9f + Rand(-0.15f, 0.15f), 3.0f + layer * 0.9f, (cell / 3 - 1) * 0.9f + Rand(-0.15f, 0.15f));
        JPH::Vec3 axis(Rand(-1, 1), Rand(-1, 1), Rand(-1, 1));
        JPH::Quat rot = axis.LengthSq() > 1e-4f ? JPH::Quat::sRotation(axis.Normalized(), Rand(0, 3.14159f)) : JPH::Quat::sIdentity();
        AddBox(half, pos, rot, JPH::Vec3::sZero(), JPH::Vec3::sZero(), true);
    }
    Hist.drops++;
    W->sinceDrop = W->asleepFor = 0;
}

// The floor and a plank the boxes slide off: the world's two static bodies.
JPH::Vec3 StaticHalf(size_t i) { return i == 0 ? JPH::Vec3(6, 0.5f, 4) : JPH::Vec3(2.2f, 0.08f, 1.6f); }

bool WorldInit() {
    // Jolt's globals live in this DLL (a static library linked into it): set up at each load, freed at each unload
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssert;)
    if (!JPH::VerifyJoltVersionID()) {   // the headers' definitions against the library's
        H->log(M, MML_LOG_ERROR, "Jolt: the headers and the library were built with other settings");
        return false;
    }
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    W = new World();
    W->temp = new JPH::TempAllocatorImpl(4 * 1024 * 1024);
    W->jobs = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, S.threads);
    W->phys = new JPH::PhysicsSystem();
    W->phys->Init(1024, 0, 4096, 4096, W->bp, W->ovb, W->pairs);
    JPH::BodyInterface &bi = W->phys->GetBodyInterface();
    auto addStatic = [&](JPH::Vec3 half, JPH::RVec3 pos, JPH::Quat rot) {
        JPH::BodyCreationSettings bs(new JPH::BoxShape(half), pos, rot, JPH::EMotionType::Static, Layers::kStatic);
        W->statics.push_back(bi.CreateAndAddBody(bs, JPH::EActivation::DontActivate));
    };
    addStatic(StaticHalf(0), JPH::RVec3(0, -0.5f, 0), JPH::Quat::sIdentity());
    addStatic(StaticHalf(1), JPH::RVec3(2.3f, 1.0f, 0), JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -0.45f));
    return true;
}

void WorldFree() {
    if (!W) return;
    RemoveBoxes();
    JPH::BodyInterface &bi = W->phys->GetBodyInterface();
    bi.RemoveBodies(W->statics.data(), static_cast<int>(W->statics.size()));
    bi.DestroyBodies(W->statics.data(), static_cast<int>(W->statics.size()));
    delete W->phys;
    delete W->jobs;    // its threads are told to stop and joined here: none is left in this DLL's code
    delete W->temp;
    delete W;
    W = nullptr;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void WorldStep(float dt) {
    if (!W) return;
    W->acc = std::min(W->acc + std::min(dt, 0.1f), 4.0 * kStep);   // at most 4 steps a frame
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    while (W->acc >= kStep) {
        W->acc -= kStep;
        QueryPerformanceCounter(&t0);
        W->phys->Update(kStep, 1, W->temp, W->jobs);
        QueryPerformanceCounter(&t1);
        float ms = static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        W->stepMs = W->steps ? W->stepMs * 0.95f + ms * 0.05f : ms;
        W->steps++;
        W->simTime += kStep;
        W->sinceDrop += kStep;
    }
    JPH::BodyInterface &bi = W->phys->GetBodyInterface();
    W->awake = 0;
    for (const JPH::BodyID &id : W->bodies) W->awake += bi.IsActive(id) ? 1 : 0;
    W->asleepFor = W->awake ? 0 : W->asleepFor + dt;
    if (W->asleepFor > 1.5 || W->sinceDrop > 30) Drop();   // all asleep (or a pile that never settles): again
}

// ---- the Direct3D 11 device
struct Gpu {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *rt = nullptr, *depth = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11ShaderResourceView *rtSrv = nullptr, *boxesSrv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    ID3D11Buffer *vb = nullptr, *ib = nullptr, *cb = nullptr, *boxes = nullptr, *counter = nullptr, *staging[3] = {};
    ID3D11UnorderedAccessView *counterUav = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11ComputeShader *cs = nullptr;
    ID3D11InputLayout *layout = nullptr;
    ID3D11RasterizerState *raster = nullptr;
    ID3D11DepthStencilState *depthState = nullptr;
    int size = 0;
    uint64_t frames = 0, read = 0, stagedAt[3] = {};
    uint32_t coveredPx = 0;
    HRESULT removed = S_OK;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    char adapter[128] = "";
    float createMs = 0;
    int made = 0;                       // devices made by this load
    double retryIn = 0;                 // a lost device is made again after this many seconds
} G;
constexpr int kMaxBoxes = 400, kMaxInstances = kMaxBoxes + 2;   // (the boxes and the two static bodies)
struct GpuBox { float world[16]; float half[3], awake; };
struct GpuFrameConsts { float viewProj[16]; float lightDir[4]; };

template <class T> void Rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

// Every object made from the device released, the device last: its count must reach 0 (else a reference leaked:
// the device and its memory would stay in the process after the DLL is gone).
int GpuRelease() {
    if (!G.dev) return 0;
    if (G.ctx) { G.ctx->ClearState(); G.ctx->Flush(); }
    Rel(G.rtv); Rel(G.rtSrv); Rel(G.rt); Rel(G.dsv); Rel(G.depth);
    Rel(G.vb); Rel(G.ib); Rel(G.cb); Rel(G.boxesSrv); Rel(G.boxes); Rel(G.counterUav); Rel(G.counter);
    for (auto *&s : G.staging) Rel(s);
    Rel(G.vs); Rel(G.ps); Rel(G.cs); Rel(G.layout); Rel(G.raster); Rel(G.depthState);
    Rel(G.ctx);
    int left = static_cast<int>(G.dev->Release());
    G.dev = nullptr;
    if (left) H->log(M, MML_LOG_ERROR, "the Direct3D 11 device still has %d references after its release: a leak", left);
    return left;
}

bool Check(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr)) return true;
    H->log(M, MML_LOG_ERROR, "Direct3D 11: %s failed (0x%08X)", what, static_cast<unsigned>(hr));
    return false;
}

bool GpuCreate() {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {S.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &G.dev, &G.level, &G.ctx);
        if (hr == E_INVALIDARG)   // a Direct3D 11.0 runtime does not know 11_1
            hr = D3D11CreateDevice(nullptr, type, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &G.dev, &G.level, &G.ctx);
        if (SUCCEEDED(hr)) break;
        H->log(M, MML_LOG_WARN, "D3D11CreateDevice (%s) failed (0x%08X)", type == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware", static_cast<unsigned>(hr));
    }
    if (!Check(hr, "D3D11CreateDevice")) return false;
    G.made++;
    Hist.devicesMade++;
    G.adapter[0] = 0;
    IDXGIDevice *dxgi = nullptr;
    if (SUCCEEDED(G.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi)))) {
        IDXGIAdapter *ad = nullptr;
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(dxgi->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&d)))
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, G.adapter, sizeof G.adapter, nullptr, nullptr);
        Rel(ad);
        Rel(dxgi);
    }
    for (char *c = G.adapter; *c; c++) if (*c == '"' || *c == '\\') *c = ' ';   // (put into JSON as it is)

    G.size = S.gpuSize;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = static_cast<UINT>(G.size);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = Check(G.dev->CreateTexture2D(&td, nullptr, &G.rt), "the picture") &&
              Check(G.dev->CreateRenderTargetView(G.rt, nullptr, &G.rtv), "its target view") &&
              Check(G.dev->CreateShaderResourceView(G.rt, nullptr, &G.rtSrv), "its resource view");
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ok = ok && Check(G.dev->CreateTexture2D(&td, nullptr, &G.depth), "the depth") &&
         Check(G.dev->CreateDepthStencilView(G.depth, nullptr, &G.dsv), "its view");

    // the cube: 6 faces of 4 vertices (position, normal), half extent 1
    struct Vtx { float p[3], n[3]; };
    std::vector<Vtx> v;
    std::vector<uint16_t> idx;
    for (int axis = 0; axis < 3; axis++)
        for (int sign = -1; sign <= 1; sign += 2) {
            float n[3] = {0, 0, 0};
            n[axis] = static_cast<float>(sign);
            int u = (axis + 1) % 3, w = (axis + 2) % 3;
            uint16_t base = static_cast<uint16_t>(v.size());
            for (int k = 0; k < 4; k++) {
                Vtx x{};
                x.p[axis] = static_cast<float>(sign);
                x.p[u] = (k & 1) ? 1.0f : -1.0f;
                x.p[w] = (k & 2) ? 1.0f : -1.0f;
                memcpy(x.n, n, sizeof n);
                v.push_back(x);
            }
            for (uint16_t k : {0, 1, 3, 0, 3, 2}) idx.push_back(static_cast<uint16_t>(base + k));
        }
    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA init{};
    bd.ByteWidth = static_cast<UINT>(v.size() * sizeof(Vtx));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    init.pSysMem = v.data();
    ok = ok && Check(G.dev->CreateBuffer(&bd, &init, &G.vb), "the vertices");
    bd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(uint16_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = idx.data();
    ok = ok && Check(G.dev->CreateBuffer(&bd, &init, &G.ib), "the indices");
    bd = {};
    bd.ByteWidth = sizeof(GpuFrameConsts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && Check(G.dev->CreateBuffer(&bd, nullptr, &G.cb), "the constants");
    bd.ByteWidth = sizeof(GpuBox) * kMaxInstances;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(GpuBox);
    ok = ok && Check(G.dev->CreateBuffer(&bd, nullptr, &G.boxes), "the boxes") &&
         Check(G.dev->CreateShaderResourceView(G.boxes, nullptr, &G.boxesSrv), "their view");
    bd = {};
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ok = ok && Check(G.dev->CreateBuffer(&bd, nullptr, &G.counter), "the counter");
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ok = ok && Check(G.dev->CreateUnorderedAccessView(G.counter, &ud, &G.counterUav), "its view");
    bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto *&s : G.staging) ok = ok && Check(G.dev->CreateBuffer(&bd, nullptr, &s), "a read-back buffer");

    // the shaders: the bytecode the build made from boxes.hlsl
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ok = ok && Check(G.dev->CreateVertexShader(kVsBox, sizeof kVsBox, nullptr, &G.vs), "VsBox") &&
         Check(G.dev->CreatePixelShader(kPsBox, sizeof kPsBox, nullptr, &G.ps), "PsBox") &&
         Check(G.dev->CreateComputeShader(kCsCoverage, sizeof kCsCoverage, nullptr, &G.cs), "CsCoverage") &&
         Check(G.dev->CreateInputLayout(il, 2, kVsBox, sizeof kVsBox, &G.layout), "the input layout");
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    ok = ok && Check(G.dev->CreateRasterizerState(&rd, &G.raster), "the rasterizer state") &&
         Check(G.dev->CreateDepthStencilState(&dd, &G.depthState), "the depth state");
    QueryPerformanceCounter(&t1);
    G.createMs = static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    G.removed = S_OK;
    if (!ok) { GpuRelease(); return false; }
    H->log(M, MML_LOG_INFO, "Direct3D 11 device on %s (feature level %d_%d), made in %.1f ms", G.adapter, G.level >> 12,
           (G.level >> 8) & 15, G.createMs);
    return true;
}

// Row vectors (as the shaders' mul(v, M)): a camera looking at the pile from the front and above.
void ViewProj(float out[16]) {
    const float eye[3] = {1.5f, 4.5f, -9.0f}, at[3] = {0.8f, 1.2f, 0};
    float z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    float l = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float &c : z) c /= l;
    float x[3] = {-z[2], 0, z[0]};                     // z x up: Jolt's world is right-handed, the projection left-handed
    l = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    x[0] /= l; x[2] /= l;
    float y[3] = {x[1] * z[2] - x[2] * z[1], x[2] * z[0] - x[0] * z[2], x[0] * z[1] - x[1] * z[0]};   // x x z: up
    auto dot = [&](const float *a) { return a[0] * eye[0] + a[1] * eye[1] + a[2] * eye[2]; };
    const float view[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x), -dot(y), -dot(z), 1};
    const float zn = 0.1f, zf = 100.0f, s = 1.0f / std::tan(0.5f * 0.9f);
    const float proj[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += view[r * 4 + k] * proj[k * 4 + c];
            out[r * 4 + c] = sum;
        }
}

void GpuFrame(float dt) {
    if (!S.gpu) return;
    if (!G.dev) {   // none yet, or lost: made again (not every frame: a driver that refuses would be asked 1000 times a second)
        if ((G.retryIn -= dt) > 0) return;
        G.retryIn = 1;
        if (!GpuCreate()) return;
    }
    HRESULT removed = G.dev->GetDeviceRemovedReason();
    if (removed != S_OK) {   // a lost device (a driver reset, a hang, the adapter gone): everything is made anew
        H->log(M, MML_LOG_ERROR, "the Direct3D 11 device was lost (0x%08X): made again in a second", static_cast<unsigned>(removed));
        G.removed = removed;
        Hist.devicesLost++;
        GpuRelease();
        G.retryIn = 1;
        return;
    }
    ID3D11DeviceContext *c = G.ctx;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(G.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto *k = static_cast<GpuFrameConsts *>(m.pData);
        ViewProj(k->viewProj);
        const float light[4] = {0.4f, -0.8f, 0.45f, 0};
        memcpy(k->lightDir, light, sizeof light);
        c->Unmap(G.cb, 0);
    }
    UINT count = 0;
    if (W && SUCCEEDED(c->Map(G.boxes, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto *b = static_cast<GpuBox *>(m.pData);
        JPH::BodyInterface &bi = W->phys->GetBodyInterface();
        auto put = [&](JPH::BodyID id, JPH::Vec3 half, float awake) {
            JPH::RMat44 t = bi.GetWorldTransform(id);
            JPH::Vec3 ax[4] = {t.GetAxisX(), t.GetAxisY(), t.GetAxisZ(), JPH::Vec3(t.GetTranslation())};
            for (int r = 0; r < 4; r++) {   // a row a Jolt column: the shaders' row vectors
                b[count].world[r * 4 + 0] = ax[r].GetX();
                b[count].world[r * 4 + 1] = ax[r].GetY();
                b[count].world[r * 4 + 2] = ax[r].GetZ();
                b[count].world[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
            }
            b[count].half[0] = half.GetX();
            b[count].half[1] = half.GetY();
            b[count].half[2] = half.GetZ();
            b[count].awake = awake;
            count++;
        };
        for (size_t i = 0; i < W->statics.size(); i++) put(W->statics[i], StaticHalf(i), -1);
        for (size_t i = 0; i < W->bodies.size() && count < kMaxInstances; i++)
            put(W->bodies[i], W->half[i], bi.IsActive(W->bodies[i]) ? 1.0f : 0.0f);
        c->Unmap(G.boxes, 0);
    }
    const float clear[4] = {0.08f, 0.09f, 0.11f, 0};   // alpha 0: what the boxes do not cover
    c->ClearRenderTargetView(G.rtv, clear);
    c->ClearDepthStencilView(G.dsv, D3D11_CLEAR_DEPTH, 1, 0);
    c->OMSetRenderTargets(1, &G.rtv, G.dsv);
    c->OMSetDepthStencilState(G.depthState, 0);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(G.size), static_cast<float>(G.size), 0, 1};
    c->RSSetViewports(1, &vp);
    c->RSSetState(G.raster);
    UINT stride = 24, offset = 0;
    c->IASetVertexBuffers(0, 1, &G.vb, &stride, &offset);
    c->IASetIndexBuffer(G.ib, DXGI_FORMAT_R16_UINT, 0);
    c->IASetInputLayout(G.layout);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(G.vs, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &G.cb);
    c->VSSetShaderResources(0, 1, &G.boxesSrv);
    c->PSSetShader(G.ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &G.cb);
    if (count) c->DrawIndexedInstanced(36, count, 0, 0, 0);
    ID3D11RenderTargetView *noRtv = nullptr;
    c->OMSetRenderTargets(1, &noRtv, nullptr);   // the picture is read next: not bound as a target any more
    // the pixels covered, counted on the GPU into the counter, copied to a read-back buffer read a few frames later
    const UINT zero[4] = {};
    c->ClearUnorderedAccessViewUint(G.counterUav, zero);
    c->CSSetShader(G.cs, nullptr, 0);
    c->CSSetShaderResources(0, 1, &G.rtSrv);
    c->CSSetUnorderedAccessViews(0, 1, &G.counterUav, nullptr);
    c->Dispatch((G.size + 7) / 8, (G.size + 7) / 8, 1);
    ID3D11ShaderResourceView *noSrv = nullptr;
    ID3D11UnorderedAccessView *noUav = nullptr;
    c->CSSetShaderResources(0, 1, &noSrv);
    c->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    int slot = static_cast<int>(G.frames % 3);
    c->CopyResource(G.staging[slot], G.counter);
    G.stagedAt[slot] = ++G.frames;
    Hist.gpuFrames++;
    // A device with no swap chain has no Present to send its commands: without a flush they wait in the driver's
    // buffer, and a read that does not wait never finds them done
    c->Flush();
    int oldest = static_cast<int>(G.frames % 3);   // the slot written two frames ago
    if (G.stagedAt[oldest] && SUCCEEDED(c->Map(G.staging[oldest], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        G.coveredPx = *static_cast<const uint32_t *>(m.pData);
        G.read = G.stagedAt[oldest];
        c->Unmap(G.staging[oldest], 0);
    }
}

// The picture as a 32-bit BMP (waits for the GPU: a command, not a frame's work).
bool Snapshot(const std::string &path, std::string *err) {
    if (!G.dev) { *err = "no device"; return false; }
    D3D11_TEXTURE2D_DESC td;
    G.rt->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *copy = nullptr;
    if (FAILED(G.dev->CreateTexture2D(&td, nullptr, &copy))) { *err = "no read-back texture"; return false; }
    G.ctx->CopyResource(copy, G.rt);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(G.ctx->Map(copy, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        const int w = static_cast<int>(td.Width), h = static_cast<int>(td.Height);
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + w * h * 4;
        ih.biSize = sizeof ih;
        ih.biWidth = w;
        ih.biHeight = h;          // bottom-up
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        FILE *f = mml_fopen(path.c_str(), "wb");
        if (f) {
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            std::vector<uint8_t> row(w * 4);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t *src = static_cast<const uint8_t *>(m.pData) + y * m.RowPitch;
                for (int x = 0; x < w; x++) {   // RGBA to the file's BGRA
                    row[x * 4 + 0] = src[x * 4 + 2];
                    row[x * 4 + 1] = src[x * 4 + 1];
                    row[x * 4 + 2] = src[x * 4 + 0];
                    row[x * 4 + 3] = 255;
                }
                fwrite(row.data(), row.size(), 1, f);
            }
            ok = fclose(f) == 0;
        } else {
            ok = false;
        }
        if (!ok) *err = "cannot write " + path;
        G.ctx->Unmap(copy, 0);
    } else {
        *err = "Map failed";
    }
    copy->Release();
    return ok;
}

// ---- the state across a reload: the counts, then the bodies (their place, turn, speeds, size, sleep)
void SaveState() {
    std::vector<uint8_t> blob(sizeof kStateVersion + sizeof Hist);
    memcpy(blob.data(), &kStateVersion, sizeof kStateVersion);
    memcpy(blob.data() + sizeof kStateVersion, &Hist, sizeof Hist);
    if (W) {
        JPH::BodyInterface &bi = W->phys->GetBodyInterface();
        for (size_t i = 0; i < W->bodies.size(); i++) {
            SavedBody s{};
            JPH::RVec3 p = bi.GetPosition(W->bodies[i]);
            JPH::Quat q = bi.GetRotation(W->bodies[i]);
            JPH::Vec3 lin = bi.GetLinearVelocity(W->bodies[i]), ang = bi.GetAngularVelocity(W->bodies[i]);
            float v[16] = {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()), static_cast<float>(p.GetZ()),
                           q.GetX(), q.GetY(), q.GetZ(), q.GetW(), lin.GetX(), lin.GetY(), lin.GetZ(),
                           ang.GetX(), ang.GetY(), ang.GetZ(), W->half[i].GetX(), W->half[i].GetY(), W->half[i].GetZ()};
            memcpy(&s, v, sizeof v);
            s.awake = bi.IsActive(W->bodies[i]) ? 1 : 0;
            const auto *b = reinterpret_cast<const uint8_t *>(&s);
            blob.insert(blob.end(), b, b + sizeof s);
        }
    }
    H->state_put(M, "world", blob.data(), blob.size());
}

size_t RestoreState() {   // the bodies restored (0: none kept, or another layout)
    size_t n = H->state_get(M, "world", nullptr, 0);
    if (n < sizeof kStateVersion + sizeof Hist) return 0;
    std::vector<uint8_t> blob(n);
    H->state_get(M, "world", blob.data(), n);
    uint32_t version;
    memcpy(&version, blob.data(), sizeof version);
    if (version != kStateVersion) return 0;
    memcpy(&Hist, blob.data() + sizeof version, sizeof Hist);
    size_t bodies = (n - sizeof version - sizeof Hist) / sizeof(SavedBody);
    if (!W || !bodies) return 0;
    const auto *saved = reinterpret_cast<const SavedBody *>(blob.data() + sizeof version + sizeof Hist);
    for (size_t i = 0; i < bodies; i++) {
        const SavedBody &s = saved[i];
        AddBox(JPH::Vec3(s.half[0], s.half[1], s.half[2]), JPH::RVec3(s.pos[0], s.pos[1], s.pos[2]),
               JPH::Quat(s.rot[0], s.rot[1], s.rot[2], s.rot[3]).Normalized(), JPH::Vec3(s.lin[0], s.lin[1], s.lin[2]),
               JPH::Vec3(s.ang[0], s.ang[1], s.ang[2]), s.awake != 0);
    }
    return bodies;
}

// ---- the overlay: the pile from the side (x across, y up; from the GPU camera's side, so +x is to the left: Jolt's
// world is right-handed), drawn with the game's 2D lines
void Draw() {
    if (!S.show || !W) return;
    const float k = S.scale, x0 = S.x, y0 = S.y, w = 290 * k, h = 150 * k, view = 64 * k;
    H->draw_rect(M, x0, y0, w, h + view, 0xA0101418u, 1);
    H->draw_rect(M, x0, y0, w, h + view, 0xFF505868u, 0);
    auto sx = [&](float wx) { return x0 + (6.0f - wx) / 12.0f * w; };   // seen from -z, as the GPU's camera sees it
    auto sy = [&](float wy) { return y0 + h - (wy + 0.6f) / 8.0f * h; };
    JPH::BodyInterface &bi = W->phys->GetBodyInterface();
    auto drawBox = [&](JPH::BodyID id, JPH::Vec3 half, uint32_t color) {
        JPH::RMat44 t = bi.GetWorldTransform(id);
        float px[8], py[8];
        for (int c = 0; c < 8; c++) {
            JPH::Vec3 corner((c & 1) ? half.GetX() : -half.GetX(), (c & 2) ? half.GetY() : -half.GetY(), (c & 4) ? half.GetZ() : -half.GetZ());
            JPH::RVec3 p = t * corner;
            px[c] = sx(static_cast<float>(p.GetX()));
            py[c] = sy(static_cast<float>(p.GetY()));
        }
        for (int a = 0; a < 8; a++)       // the 12 edges: corners that differ in one bit
            for (int bit = 1; bit < 8; bit <<= 1)
                if (!(a & bit)) H->draw_line(M, px[a], py[a], px[a | bit], py[a | bit], color);
    };
    for (size_t i = 0; i < W->statics.size(); i++)
        drawBox(W->statics[i], StaticHalf(i), 0xFF707880u);
    for (size_t i = 0; i < W->bodies.size() && i < 40; i++)   // (40 boxes: 480 lines; the frame takes 4096 items)
        drawBox(W->bodies[i], W->half[i], bi.IsActive(W->bodies[i]) ? 0xFFFFC040u : 0xFF8090A0u);
    char line[160];
    const float ty = y0 + h + 4 * k, size = 13 * k;
    snprintf(line, sizeof line, "Jolt: %d boxes, %d awake, a step %.2f ms (%d threads)", static_cast<int>(W->bodies.size()), W->awake, W->stepMs, S.threads);
    H->draw_text(M, x0 + 6 * k, ty, size, 0xFFFFFFFFu, MML_FONT_SMALL, MML_DRAW_LEFT, line);
    if (!S.gpu) snprintf(line, sizeof line, "Direct3D 11: off");
    else if (!G.dev) snprintf(line, sizeof line, "Direct3D 11: no device (0x%08X)", static_cast<unsigned>(G.removed));
    else snprintf(line, sizeof line, "D3D11 frame %llu: %.1f %% covered", static_cast<unsigned long long>(G.frames), 100.0 * G.coveredPx / (G.size * G.size));
    H->draw_text(M, x0 + 6 * k, ty + 15 * k, size, G.dev || !S.gpu ? 0xFFFFFFFFu : 0xFFFF6060u, MML_FONT_SMALL, MML_DRAW_LEFT, line);
    snprintf(line, sizeof line, "load %u, devices made %u, lost %u", Hist.loads, Hist.devicesMade, Hist.devicesLost);
    H->draw_text(M, x0 + 6 * k, ty + 30 * k, size, 0xFFB0B8C0u, MML_FONT_SMALL, MML_DRAW_LEFT, line);
}

void OnFrame(const MmlEvent *ev, void *) {
    float dt = ev->dt > 0 ? ev->dt : 0.016f;
    if (H->mission_state() != MML_MISSION_PAUSED) WorldStep(dt);   // (its own world: it runs in the menus too)
    GpuFrame(dt);
    Draw();
}

void OnDropKey(void *) { if (W) Drop(); }

void ReadSettings() {
    Settings old = S;
    S.boxes = std::max(1, std::min(kMaxBoxes, static_cast<int>(H->config_number(M, "boxes", 48))));
    S.threads = std::max(0, std::min(8, static_cast<int>(H->config_number(M, "threads", 2))));
    S.gpu = H->config_number(M, "gpu", 1) != 0;
    S.gpuSize = std::max(16, std::min(2048, static_cast<int>(H->config_number(M, "gpu_size", 256))));
    S.warp = H->config_number(M, "warp", 0) != 0;
    S.show = H->config_number(M, "overlay.show", 1) != 0;
    S.x = static_cast<float>(H->config_number(M, "overlay.x", 20));
    S.y = static_cast<float>(H->config_number(M, "overlay.y", 150));
    S.scale = static_cast<float>(H->config_number(M, "overlay.scale", 1));
    H->hotkey(M, H->config_string(M, "hotkey_drop", "F10"), OnDropKey, nullptr);
    if (!W) return;   // (the first read, before the world)
    if (S.boxes != old.boxes) Drop();
    if (S.threads != old.threads) {   // the pool made again with the new count (its threads joined first)
        delete W->jobs;
        W->jobs = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, S.threads);
    }
    if (S.gpu != old.gpu || S.gpuSize != old.gpuSize || S.warp != old.warp) { GpuRelease(); G.retryIn = 0; }
}

void OnConfig(const MmlEvent *, void *) { ReadSettings(); }

int Status(const char *, MmlReply *r, void *) {
    char out[1400];
    snprintf(out, sizeof out,
             "{\"load\":%u,\"boxes\":%d,\"awake\":%d,\"threads\":%d,\"sim_s\":%.2f,\"steps\":%llu,\"step_ms\":%.3f,"
             "\"restored\":%s,\"drops\":%u,"
             "\"gpu\":{\"on\":%s,\"device\":%s,\"adapter\":\"%s\",\"feature_level\":\"%d_%d\",\"size\":%d,\"frames\":%llu,"
             "\"read_frame\":%llu,\"covered_px\":%u,\"covered_pct\":%.2f,\"removed_reason\":\"0x%08X\",\"create_ms\":%.1f,"
             "\"made_this_load\":%d},"
             "\"history\":{\"loads\":%u,\"devices_made\":%u,\"devices_lost\":%u,\"gpu_frames\":%llu,\"last_unload_refs_left\":%d}}",
             Hist.loads, W ? static_cast<int>(W->bodies.size()) : 0, W ? W->awake : 0, S.threads, W ? W->simTime : 0,
             static_cast<unsigned long long>(W ? W->steps : 0), W ? W->stepMs : 0, W && W->restored ? "true" : "false", Hist.drops,
             S.gpu ? "true" : "false", G.dev ? "true" : "false", G.adapter, G.level >> 12, (G.level >> 8) & 15, G.size, static_cast<unsigned long long>(G.frames),
             static_cast<unsigned long long>(G.read), G.coveredPx, G.size ? 100.0 * G.coveredPx / (G.size * G.size) : 0.0,
             static_cast<unsigned>(G.dev ? G.dev->GetDeviceRemovedReason() : G.removed), G.createMs, G.made, Hist.loads,
             Hist.devicesMade, Hist.devicesLost, static_cast<unsigned long long>(Hist.gpuFrames), Hist.lastRefsLeft);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

int DropCmd(const char *, MmlReply *r, void *) {
    if (!W) { H->reply_error(r, "no world"); return 1; }
    Drop();
    H->reply_append(r, "true", static_cast<size_t>(-1));
    return 0;
}

int SnapshotCmd(const char *, MmlReply *r, void *) {
    std::string path = std::string(H->mod_data_dir(M)) + "snapshot.bmp", err;
    if (!Snapshot(path, &err)) { H->reply_error(r, err.c_str()); return 1; }
    for (char &c : path) if (c == '\\') c = '/';
    char out[MAX_PATH + 96];
    snprintf(out, sizeof out, "{\"path\":\"%s\",\"size\":%d,\"covered_px\":%u}", path.c_str(), G.size, G.coveredPx);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    ReadSettings();
    if (!WorldInit()) return 1;
    size_t kept = RestoreState();   // the last load's counts and bodies, if it left them
    W->restored = kept > 0;
    if (!kept) Drop();
    Hist.loads++;
    G.retryIn = 0;
    if (S.gpu) GpuCreate();         // here, so a device that cannot be made is told at the load (GpuFrame tries again)
    H->subscribe(M, MML_EVENT_FRAME, OnFrame, nullptr);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnConfig, nullptr);
    H->command(M, "status", "the Jolt world, the Direct3D 11 device, the counts across loads", Status, nullptr);
    H->command(M, "drop", "the boxes dropped again", DropCmd, nullptr);
    H->command(M, "snapshot", "the GPU's picture of the boxes as a BMP in the data folder", SnapshotCmd, nullptr);
    H->log(M, MML_LOG_INFO, "load %u: %d boxes (%s), %d job threads, Direct3D 11 %s", Hist.loads, static_cast<int>(W->bodies.size()),
           kept ? "kept from the last load" : "dropped", S.threads, G.dev ? G.adapter : (S.gpu ? "failed" : "off"));
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    Hist.lastRefsLeft = GpuRelease();   // first: the device's count, told to the next load
    SaveState();
    WorldFree();                        // the job threads joined, Jolt's factory and types freed
}
