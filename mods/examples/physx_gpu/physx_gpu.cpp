// physx_gpu: a mod in two halves (docs/MODDING.md "64-bit halves") doing what a 32-bit game cannot: PhysX 5's GPU part
// (CUDA) in the game's world. Its 64-bit half (physx_gpu64.cpp) steps water (PBD particles), a cloth (FEM) and a jelly
// (an FEM soft body) on the GPU and copies their points into a shared texture without the CPU; this 32-bit half asks
// for them, gives the 64-bit half the game's world (the static collision around them, the people and cars moving
// there) and draws the points into the frame with the graphics bridge (physx_gpu.hlsl): the water as a surface
// (screen-space fluid: refraction, reflection, depth), the cloth and the jelly as lit meshes, hidden by the world.
//
//   call physx_gpu.pour  {"count": 32768, "ahead": 4, "height": 1.5, "spacing": 0.1}  a block of water in front
//   call physx_gpu.cloth {"count": 64, "size": 4, "ahead": 0, "height": 3}            a sheet dropped from above
//   call physx_gpu.jelly {"size": 1.5, "ahead": 4, "height": 2, "stiffness": 50000}   a soft cube dropped
//   call physx_gpu.clear
//   call physx_gpu.wait  {"ms": 8}   how long the pass waits for this frame's step (channel_wait; 0: draws the latest)
//   call physx_gpu.state -> {"frames", "late": {"0", "1", "2+"}, "none", "round_trip_ms", "ask_to_done_ms",
//                           "step_ms", "copy_ms", "waited", "fluid", "cloth", "jelly", "draw_cpu_ms", "error"...}
//   call physx_gpu.side  -> the 64-bit half's own count (physx_gpu64.cpp)
//
// The frame: at FRAME this half writes "frame" (the frame, its dt, the movers in the region); the 64-bit half steps it
// on the GPU and names the texture holding that step's points in "out"; this half's pass (after the world is drawn,
// the same frame) draws them and counts how many frames late they were. The GPU's step takes a few milliseconds and
// shares the GPU with the game's drawing: when it is not over by the pass, the pass waits for it (channel_wait), at
// most a budget of milliseconds, then draws the latest one.
//   call physx_gpu.points -> where the water's particles are, read back from the GPU after a step (each call answers
//                           the last measure and asks for the next)
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/mml.h"
#include "physx_gpu.h"
#include "shaders/VsMesh.h"
#include "shaders/PsMesh.h"
#include "shaders/CsNormals.h"
#include "shaders/VsSplat.h"
#include "shaders/PsSplatDepth.h"
#include "shaders/PsSplatThick.h"
#include "shaders/PsSmooth.h"
#include "shaders/PsFluid.h"

using nlohmann::json;

namespace {
const MmlHost *H;
MmlMod *M;
MmlChannel *g_opsCh, *g_frameCh, *g_outCh, *g_topoCh;
const float kRegionHalf = 24, kRegionDown = 8, kRegionUp = 16;

std::unique_ptr<gpux::Ops> g_ops = std::make_unique<gpux::Ops>();
std::unique_ptr<gpux::Topology> g_topo = std::make_unique<gpux::Topology>();
gpux::Frame g_frameRec;
gpux::Out g_out;
std::vector<MmlShape> g_shapes;
uint32_t g_serial, g_region;
bool g_haveRegion;
uint64_t g_frame;
double g_freqMs;
uint32_t g_waitMs = 8;   // the pass's budget for this frame's step (physx_gpu.wait)

struct Stats {
    uint64_t frames, late0, late1, late2, none;
    double rtSum, rtMax, workSum, stepSum, copySum, drawSum, waitSum, waitMax;
    uint64_t waited;
} S;

uint64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
double Ms(int64_t from, int64_t to) { return (to - from) / g_freqMs; }

// ---- asking: an op in the ring of the last kOps, with the region's statics
bool Ground(const float at[3], float *y) {
    const float from[3] = {at[0], at[1] + 3, at[2]}, to[3] = {at[0], at[1] - 12, at[2]};
    MmlHit hit{};
    hit.size = sizeof hit;
    if (!H->world_ray(M, from, to, MML_WORLD_STATIC, nullptr, &hit)) return false;
    *y = hit.pos[1];
    return true;
}

// The region around a place (taken anew when the place is out of its middle half), its static world into the record.
void Region(const float at[3], float groundY) {
    gpux::Ops &o = *g_ops;
    if (g_haveRegion && std::fabs(at[0] - (o.lo[0] + o.hi[0]) * 0.5f) < kRegionHalf * 0.5f &&
        std::fabs(at[2] - (o.lo[2] + o.hi[2]) * 0.5f) < kRegionHalf * 0.5f)
        return;
    o.lo[0] = at[0] - kRegionHalf, o.lo[1] = groundY - kRegionDown, o.lo[2] = at[2] - kRegionHalf;
    o.hi[0] = at[0] + kRegionHalf, o.hi[1] = groundY + kRegionUp, o.hi[2] = at[2] + kRegionHalf;
    g_shapes.resize(gpux::kMaxStatics);
    int n = std::min(H->world_shapes(M, o.lo, o.hi, MML_WORLD_STATIC, g_shapes.data(), gpux::kMaxStatics), static_cast<int>(gpux::kMaxStatics));
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        const MmlShape &sh = g_shapes[i];
        gpux::Static &st = o.shape[k];
        st = gpux::Static{};
        switch (sh.kind) {
        case MML_SHAPE_TRIANGLE:
            st.kind = gpux::kTriangle;
            memcpy(st.v, sh.corner, 9 * sizeof(float));
            memcpy(st.v + 9, sh.normal, 3 * sizeof(float));
            break;
        case MML_SHAPE_BOX:
            st.kind = gpux::kBox;
            memcpy(st.v, sh.center, 3 * sizeof(float));
            memcpy(st.v + 3, sh.half, 3 * sizeof(float));
            memcpy(st.v + 6, sh.axes, 9 * sizeof(float));
            break;
        case MML_SHAPE_SPHERE:
            st.kind = gpux::kSphere;
            memcpy(st.v, sh.center, 3 * sizeof(float));
            st.v[3] = sh.half[0];
            break;
        case MML_SHAPE_POLE:
            st.kind = gpux::kPole;
            st.v[0] = sh.center[0], st.v[1] = sh.center[2], st.v[3] = sh.half[0];
            break;
        default: continue;
        }
        k++;
    }
    o.statics = k;
    o.region = ++g_region;
    g_haveRegion = true;
}

void Send(gpux::Op op) {
    op.serial = ++g_serial;
    gpux::Ops &o = *g_ops;
    gpux::Op *slot = &o.op[0];
    for (gpux::Op &s : o.op)
        if (s.serial < slot->serial) slot = &s;   // (the oldest one makes room)
    *slot = op;
    H->channel_write(g_opsCh, &o, static_cast<uint32_t>(offsetof(gpux::Ops, shape) + o.statics * sizeof(gpux::Static)));
}

// Where the player is and faces (his model's world matrix): false with no mission.
bool Player(float pos[3], float dir[3]) {
    if (H->mission_state() != MML_MISSION_RUNNING) return false;
    void *p = H->human_player(M);
    float w[16];
    if (!p || !H->frame_world(M, H->actor_frame(M, p), w)) return false;
    float fx = w[8], fz = w[10], fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 0.01f) fx = 0, fz = 1, fl = 1;
    pos[0] = w[12], pos[1] = w[13], pos[2] = w[14];
    dir[0] = fx / fl, dir[1] = 0, dir[2] = fz / fl;
    return true;
}

std::string Ask(uint32_t kind, const json &a) {
    float pos[3], dir[3];
    if (!Player(pos, dir)) return "no mission running";
    gpux::Op op{};
    op.kind = kind;
    memcpy(op.dir, dir, sizeof dir);
    float ahead = a.value("ahead", kind == gpux::kOpCloth ? 0.0f : 4.0f);
    float at[3] = {pos[0] + dir[0] * ahead, pos[1], pos[2] + dir[2] * ahead};
    float groundY = pos[1];
    Ground(at, &groundY);
    switch (kind) {
    case gpux::kOpPour:
        op.count = std::min(a.value("count", 32768u), gpux::kMaxFluid);
        op.param = a.value("spacing", 0.1f);
        at[1] = groundY + a.value("height", 1.5f);   // (the block's bottom)
        break;
    case gpux::kOpCloth:
        op.count = std::clamp(a.value("count", 64u), 8u, 128u);
        op.size = a.value("size", 4.0f);
        at[1] = std::max(groundY, pos[1]) + a.value("height", 3.0f);
        break;
    case gpux::kOpJelly:
        op.size = a.value("size", 1.5f);
        op.param = a.value("stiffness", 5.e4f);
        at[1] = groundY + a.value("height", 2.0f) + op.size * 0.5f;   // (its centre)
        break;
    default: break;
    }
    memcpy(op.at, at, sizeof at);
    if (kind != gpux::kOpClear) Region(at, groundY);
    Send(op);
    return "";
}

// ---- the frame's state out: the movers in the region (as physx_crates: a person a capsule from his narrow sphere, a
// car its whole hull)
void OnFrame(float dt) {
    g_frame++;
    if (!g_haveRegion) return;
    gpux::Frame &f = g_frameRec;
    f.frame = g_frame;
    f.qpc = static_cast<int64_t>(Qpc());
    f.dt = dt;
    f.run = H->mission_state() == MML_MISSION_RUNNING;
    f.lastOp = g_serial;
    f.movers = 0;
    int n = std::min(H->world_shapes(M, g_ops->lo, g_ops->hi, MML_WORLD_MOVING, g_shapes.data(), gpux::kMaxMovers), static_cast<int>(gpux::kMaxMovers));
    for (int i = 0; i < n; i++) {
        const MmlShape &sh = g_shapes[i];
        if (sh.kind != MML_SHAPE_BOX && sh.kind != MML_SHAPE_SPHERE) continue;
        bool person = sh.classes & 0x100, car = sh.classes & 0x200;
        if ((person && !(sh.classes & 0x1)) || (car && !(sh.classes & 0x2))) continue;
        gpux::Mover &m = f.mover[f.movers++];
        m = gpux::Mover{};
        m.id = reinterpret_cast<uintptr_t>(sh.prim);
        m.kind = sh.kind == MML_SHAPE_BOX ? gpux::kBox : gpux::kSphere;
        memcpy(m.center, sh.center, sizeof m.center);
        memcpy(m.half, sh.half, sizeof m.half);
        memcpy(m.axes, sh.axes, sizeof m.axes);
        if (person && m.kind == gpux::kSphere) {   // (the sphere 0.8 m over his feet: a capsule from 0.05 m to 1.8 m)
            float feet = sh.center[1] - 0.8f, r = sh.half[0];
            m.kind = gpux::kCapsule;
            m.center[1] = feet + 0.925f;
            m.half[1] = 0.875f - r;
        }
    }
    H->channel_write(g_frameCh, &f, static_cast<uint32_t>(offsetof(gpux::Frame, mover) + f.movers * sizeof(gpux::Mover)));
}

// ---- drawing (the bridge's Direct3D 11 device)
template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
struct Target {
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    void Free() { SafeRelease(srv), SafeRelease(rtv), SafeRelease(tex); }
};
struct Mesh {   // the cloth's or the jelly's: its indices, adjacency and normals
    uint32_t tris = 0, verts = 0;
    ID3D11Buffer *index = nullptr, *start = nullptr, *adj = nullptr, *normals = nullptr;
    ID3D11ShaderResourceView *indexSrv = nullptr, *startSrv = nullptr, *adjSrv = nullptr, *normalsSrv = nullptr;
    ID3D11UnorderedAccessView *normalsUav = nullptr;
    void Free() {
        SafeRelease(indexSrv), SafeRelease(startSrv), SafeRelease(adjSrv), SafeRelease(normalsSrv), SafeRelease(normalsUav);
        SafeRelease(index), SafeRelease(start), SafeRelease(adj), SafeRelease(normals);
        tris = verts = 0;
    }
};
struct Gfx {
    unsigned generation = 0, w = 0, h = 0;
    ID3D11VertexShader *vsMesh = nullptr, *vsSplat = nullptr;
    ID3D11PixelShader *psMesh = nullptr, *psDepth = nullptr, *psThick = nullptr, *psSmooth = nullptr, *psFluid = nullptr;
    ID3D11ComputeShader *csNormals = nullptr;
    ID3D11Buffer *cb = nullptr;
    ID3D11BlendState *add = nullptr, *opaque = nullptr, *over = nullptr;   // (over: premultiplied, the water's surface)
    ID3D11DepthStencilState *depthOn = nullptr, *depthOff = nullptr;
    ID3D11RasterizerState *noCull = nullptr;
    Target meshZ, fluidZ, fluidZ2, thick;
    ID3D11Texture2D *ds = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    Mesh cloth, jelly;
    uint32_t topology = 0;
    void FreeSized() { meshZ.Free(), fluidZ.Free(), fluidZ2.Free(), thick.Free(), SafeRelease(dsv), SafeRelease(ds); w = h = 0; }
    void Free() {
        FreeSized();
        cloth.Free(), jelly.Free();
        topology = 0;
        SafeRelease(vsMesh), SafeRelease(vsSplat), SafeRelease(psMesh), SafeRelease(psDepth), SafeRelease(psThick), SafeRelease(psSmooth);
        SafeRelease(psFluid), SafeRelease(csNormals), SafeRelease(cb), SafeRelease(add), SafeRelease(opaque), SafeRelease(over), SafeRelease(depthOn);
        SafeRelease(depthOff), SafeRelease(noCull);
    }
} G;
struct Cb {
    float origin[4];
    uint32_t rows[4];
    float water[4];
};

bool MakeTarget(ID3D11Device *d, Target &t, DXGI_FORMAT f, unsigned w, unsigned h) {
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    return SUCCEEDED(d->CreateTexture2D(&td, nullptr, &t.tex)) && SUCCEEDED(d->CreateRenderTargetView(t.tex, nullptr, &t.rtv)) &&
           SUCCEEDED(d->CreateShaderResourceView(t.tex, nullptr, &t.srv));
}

bool Make(ID3D11Device *d) {
    G.Free();
    D3D11_BUFFER_DESC bd{sizeof(Cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_BLEND_DESC add{};
    add.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    D3D11_BLEND_DESC over{};
    over.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    D3D11_BLEND_DESC opaque{};
    opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC on{TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS}, off{FALSE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_ALWAYS};
    D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    rd.DepthClipEnable = TRUE;
    return SUCCEEDED(d->CreateVertexShader(kVsMesh, sizeof kVsMesh, nullptr, &G.vsMesh)) &&
           SUCCEEDED(d->CreateVertexShader(kVsSplat, sizeof kVsSplat, nullptr, &G.vsSplat)) &&
           SUCCEEDED(d->CreatePixelShader(kPsMesh, sizeof kPsMesh, nullptr, &G.psMesh)) &&
           SUCCEEDED(d->CreatePixelShader(kPsSplatDepth, sizeof kPsSplatDepth, nullptr, &G.psDepth)) &&
           SUCCEEDED(d->CreatePixelShader(kPsSplatThick, sizeof kPsSplatThick, nullptr, &G.psThick)) &&
           SUCCEEDED(d->CreatePixelShader(kPsSmooth, sizeof kPsSmooth, nullptr, &G.psSmooth)) &&
           SUCCEEDED(d->CreatePixelShader(kPsFluid, sizeof kPsFluid, nullptr, &G.psFluid)) &&
           SUCCEEDED(d->CreateComputeShader(kCsNormals, sizeof kCsNormals, nullptr, &G.csNormals)) &&
           SUCCEEDED(d->CreateBuffer(&bd, nullptr, &G.cb)) && SUCCEEDED(d->CreateBlendState(&add, &G.add)) &&
           SUCCEEDED(d->CreateBlendState(&opaque, &G.opaque)) && SUCCEEDED(d->CreateBlendState(&over, &G.over)) && SUCCEEDED(d->CreateDepthStencilState(&on, &G.depthOn)) &&
           SUCCEEDED(d->CreateDepthStencilState(&off, &G.depthOff)) && SUCCEEDED(d->CreateRasterizerState(&rd, &G.noCull));
}

bool MakeSized(ID3D11Device *d, unsigned w, unsigned h) {
    G.FreeSized();
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0};
    if (!MakeTarget(d, G.meshZ, DXGI_FORMAT_R32_FLOAT, w, h) || !MakeTarget(d, G.fluidZ, DXGI_FORMAT_R32_FLOAT, w, h) ||
        !MakeTarget(d, G.fluidZ2, DXGI_FORMAT_R32_FLOAT, w, h) || !MakeTarget(d, G.thick, DXGI_FORMAT_R16_FLOAT, w, h) ||
        FAILED(d->CreateTexture2D(&td, nullptr, &G.ds)) || FAILED(d->CreateDepthStencilView(G.ds, nullptr, &G.dsv)))
        return false;
    G.w = w, G.h = h;
    return true;
}

ID3D11Buffer *TypedBuffer(ID3D11Device *d, const uint32_t *data, uint32_t n, UINT bind, ID3D11ShaderResourceView **srv) {
    ID3D11Buffer *b = nullptr;
    D3D11_BUFFER_DESC bd{std::max(n, 1u) * 4, D3D11_USAGE_IMMUTABLE, bind | D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init{data};
    std::vector<uint32_t> one(1, 0);
    if (!n) init.pSysMem = one.data();
    if (FAILED(d->CreateBuffer(&bd, &init, &b))) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R32_UINT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.NumElements = std::max(n, 1u);
    d->CreateShaderResourceView(b, &vd, srv);
    return b;
}

// A mesh's buffers from its triangles: the index buffer (also read by CsNormals), each vertex's triangles (CSR), its
// normals (written by CsNormals each frame).
void MakeMesh(ID3D11Device *d, Mesh &m, const uint32_t *idx, uint32_t tris, uint32_t verts) {
    m.Free();
    if (!tris || !verts) return;
    std::vector<uint32_t> start(verts + 1, 0), adj(tris * 3);
    for (uint32_t i = 0; i < tris * 3; i++)
        if (idx[i] < verts) start[idx[i] + 1]++;
    for (uint32_t v = 0; v < verts; v++) start[v + 1] += start[v];
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (uint32_t i = 0; i < tris * 3; i++)
        if (idx[i] < verts) adj[fill[idx[i]]++] = i / 3;
    m.index = TypedBuffer(d, idx, tris * 3, D3D11_BIND_INDEX_BUFFER, &m.indexSrv);
    m.start = TypedBuffer(d, start.data(), verts + 1, 0, &m.startSrv);
    m.adj = TypedBuffer(d, adj.data(), start[verts], 0, &m.adjSrv);
    D3D11_BUFFER_DESC nd{verts * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
    if (FAILED(d->CreateBuffer(&nd, nullptr, &m.normals))) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.NumElements = verts;
    d->CreateShaderResourceView(m.normals, &vd, &m.normalsSrv);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = verts;
    d->CreateUnorderedAccessView(m.normals, &ud, &m.normalsUav);
    m.tris = tris, m.verts = verts;
}

float g_pxPerMetre;   // a metre at 1 m on the screen, in pixels (the projection's _22 times half the height)
void SetCb(ID3D11DeviceContext *c, uint32_t first, uint32_t count, uint32_t dir, uint32_t kind) {
    Cb k{};
    memcpy(k.origin, g_out.origin, 12);
    k.origin[3] = g_out.radius;
    k.rows[0] = first, k.rows[1] = count, k.rows[2] = dir, k.rows[3] = kind;
    k.water[3] = 1.8f;   // (the splats' radius over the particles': a sheet one particle thick drawn whole)
    const float splat = g_out.radius * k.water[3];
    k.water[0] = g_pxPerMetre * splat * 4;   // (the smoothing's radius in pixels at 1 m: two splats across)
    k.water[1] = 1.f / (splat * splat * 4);
    k.water[2] = 0.6f;
    c->UpdateSubresource(G.cb, 0, nullptr, &k, 0, 0);
}

void DrawMesh(ID3D11DeviceContext *c, Mesh &m, uint32_t row, uint32_t verts, uint32_t kind) {
    if (!m.tris || verts != m.verts) return;
    SetCb(c, row * gpux::kPointsPerRow, verts, 0, kind);
    // the normals
    ID3D11ShaderResourceView *srvs[3] = {m.indexSrv, m.startSrv, m.adjSrv};
    c->CSSetShaderResources(8, 3, srvs);
    c->CSSetUnorderedAccessViews(0, 1, &m.normalsUav, nullptr);
    c->CSSetShader(G.csNormals, nullptr, 0);
    c->Dispatch((verts + 63) / 64, 1, 1);
    ID3D11UnorderedAccessView *noUav = nullptr;
    c->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    ID3D11ShaderResourceView *none3[3] = {};
    c->CSSetShaderResources(8, 3, none3);
    // the triangles
    c->VSSetShaderResources(4, 1, &m.normalsSrv);
    c->IASetIndexBuffer(m.index, DXGI_FORMAT_R32_UINT, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(G.vsMesh, nullptr, 0);
    c->PSSetShader(G.psMesh, nullptr, 0);
    c->DrawIndexed(m.tris * 3, 0, 0);
    ID3D11ShaderResourceView *none = nullptr;
    c->VSSetShaderResources(4, 1, &none);
}

void FullScreen(ID3D11DeviceContext *c, const MmlGfxFrame *f, ID3D11PixelShader *ps) {
    c->VSSetShader(static_cast<ID3D11VertexShader *>(f->full_screen), nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->Draw(3, 0);
}

void Pass(const MmlGfxFrame *f, void *) {
    if (f->in_view) return;
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *c = static_cast<ID3D11DeviceContext *>(f->context);
    if (G.generation != f->generation || !G.vsMesh) {
        if (!Make(d)) { G.Free(); return; }
        G.generation = f->generation;
    }
    if ((G.w != f->width || G.h != f->height) && !MakeSized(d, f->width, f->height)) return;
    if (!g_haveRegion) return;
    g_pxPerMetre = f->proj[5] * f->height * 0.5f;
    S.frames++;
    // this frame's step: waited for (channel_wait) while it is not there, within the budget; else the latest one
    uint64_t v = 0;
    bool have = H->channel_read(g_outCh, &g_out, sizeof g_out, &v) == sizeof g_out;
    if (g_waitMs && (!have || g_out.frame < g_frame)) {
        LARGE_INTEGER w0, w1;
        QueryPerformanceCounter(&w0);
        double left = g_waitMs;
        while (left > 0 && (!have || g_out.frame < g_frame)) {
            uint64_t nv = H->channel_wait(g_outCh, v, static_cast<uint32_t>(std::ceil(left)));
            if (!nv) break;
            have = H->channel_read(g_outCh, &g_out, sizeof g_out, &v) == sizeof g_out;
            QueryPerformanceCounter(&w1);
            left = g_waitMs - Ms(w0.QuadPart, w1.QuadPart);
        }
        QueryPerformanceCounter(&w1);
        double waited = Ms(w0.QuadPart, w1.QuadPart);
        S.waited++;
        S.waitSum += waited;
        S.waitMax = std::max(S.waitMax, waited);
    }
    if (!have || g_out.texture > 1) { S.none++; return; }
    MmlSharedTexture t{};
    t.size = sizeof t;
    if (!H->gfx_shared_texture(M, g_out.texture ? "pos1" : "pos0", &t) || !t.srv) { S.none++; return; }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    uint64_t late = g_frame - g_out.frame;
    (late == 0 ? S.late0 : late == 1 ? S.late1 : S.late2)++;
    double rt = Ms(g_out.askQpc, t0.QuadPart);
    S.rtSum += rt;
    S.rtMax = std::max(S.rtMax, rt);
    S.workSum += Ms(g_out.askQpc, g_out.doneQpc);
    S.stepSum += g_out.stepMs;
    S.copySum += g_out.copyMs;
    if (g_out.topology != G.topology) {   // the cloth's and the jelly's triangles, new
        uint64_t tv = 0;
        if (H->channel_read(g_topoCh, g_topo.get(), sizeof(gpux::Topology), &tv) >= offsetof(gpux::Topology, index) && g_topo->version == g_out.topology) {
            MakeMesh(d, G.cloth, g_topo->index, g_topo->clothTris, g_out.cloth);
            MakeMesh(d, G.jelly, g_topo->index + 3 * g_topo->clothTris, g_topo->jellyTris, g_out.jelly);
            G.topology = g_out.topology;
        }
    }
    auto *srv = static_cast<ID3D11ShaderResourceView *>(t.srv);
    ID3D11ShaderResourceView *base[3] = {static_cast<ID3D11ShaderResourceView *>(f->depth), static_cast<ID3D11ShaderResourceView *>(f->color), nullptr};
    ID3D11Buffer *cbs[2] = {static_cast<ID3D11Buffer *>(f->constants), G.cb};
    for (auto set : {&ID3D11DeviceContext::VSSetConstantBuffers, &ID3D11DeviceContext::PSSetConstantBuffers, &ID3D11DeviceContext::CSSetConstantBuffers})
        (c->*set)(0, 2, cbs);
    c->VSSetShaderResources(0, 2, base);
    c->CSSetShaderResources(0, 2, base);
    c->VSSetShaderResources(3, 1, &srv);
    c->PSSetShaderResources(3, 1, &srv);
    c->CSSetShaderResources(3, 1, &srv);
    c->IASetInputLayout(nullptr);
    c->RSSetState(G.noCull);
    auto *target = static_cast<ID3D11RenderTargetView *>(f->target);
    const float far4[4] = {1e9f, 1e9f, 1e9f, 1e9f}, zero4[4] = {};
    // the meshes: into the effects target, their linear depth beside it
    c->ClearRenderTargetView(G.meshZ.rtv, far4);
    c->ClearDepthStencilView(G.dsv, D3D11_CLEAR_DEPTH, 1, 0);
    if ((G.cloth.tris && g_out.cloth) || (G.jelly.tris && g_out.jelly)) {
        ID3D11RenderTargetView *rts[2] = {target, G.meshZ.rtv};
        c->OMSetRenderTargets(2, rts, G.dsv);
        c->OMSetDepthStencilState(G.depthOn, 0);
        c->OMSetBlendState(G.opaque, nullptr, 0xFFFFFFFF);
        DrawMesh(c, G.cloth, gpux::kClothRow, g_out.cloth, 0);
        DrawMesh(c, G.jelly, gpux::kJellyRow, g_out.jelly, 1);
    }
    // the water
    if (g_out.fluid) {
        ID3D11ShaderResourceView *meshZ = G.meshZ.srv;
        c->OMSetRenderTargets(1, &G.fluidZ.rtv, G.dsv);
        c->ClearRenderTargetView(G.fluidZ.rtv, far4);
        c->ClearDepthStencilView(G.dsv, D3D11_CLEAR_DEPTH, 1, 0);
        c->PSSetShaderResources(5, 1, &meshZ);
        c->OMSetDepthStencilState(G.depthOn, 0);
        c->OMSetBlendState(G.opaque, nullptr, 0xFFFFFFFF);
        SetCb(c, gpux::kFluidRow * gpux::kPointsPerRow, g_out.fluid, 0, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(G.vsSplat, nullptr, 0);
        c->PSSetShader(G.psDepth, nullptr, 0);
        c->Draw(g_out.fluid * 6, 0);
        // its thickness, added up
        c->OMSetRenderTargets(1, &G.thick.rtv, nullptr);
        c->ClearRenderTargetView(G.thick.rtv, zero4);
        c->OMSetDepthStencilState(G.depthOff, 0);
        c->OMSetBlendState(G.add, nullptr, 0xFFFFFFFF);
        c->PSSetShader(G.psThick, nullptr, 0);
        c->Draw(g_out.fluid * 6, 0);
        // the depth smoothed: x into fluidZ2, y back into fluidZ
        c->OMSetBlendState(G.opaque, nullptr, 0xFFFFFFFF);
        for (int pass = 0; pass < 2; pass++) {
            Target &from = pass ? G.fluidZ2 : G.fluidZ, &to = pass ? G.fluidZ : G.fluidZ2;
            ID3D11ShaderResourceView *none = nullptr;
            c->PSSetShaderResources(6, 1, &none);
            c->OMSetRenderTargets(1, &to.rtv, nullptr);
            c->PSSetShaderResources(6, 1, &from.srv);
            SetCb(c, 0, g_out.fluid, pass, 0);
            FullScreen(c, f, G.psSmooth);
        }
        // the surface, over the frame (the bridge's blending: premultiplied over)
        ID3D11ShaderResourceView *none = nullptr;
        c->PSSetShaderResources(6, 1, &none);
        c->OMSetRenderTargets(1, &target, nullptr);
        ID3D11ShaderResourceView *fl[2] = {G.fluidZ.srv, G.thick.srv};
        c->PSSetShaderResources(6, 2, fl);
        c->OMSetBlendState(G.over, nullptr, 0xFFFFFFFF);   // (premultiplied over: its thin edge fades into what is under it)
        FullScreen(c, f, G.psFluid);
    }
    ID3D11ShaderResourceView *clear[8] = {};
    c->OMSetRenderTargets(1, &target, nullptr);
    c->PSSetShaderResources(3, 5, clear);
    c->VSSetShaderResources(3, 2, clear);
    c->CSSetShaderResources(3, 1, clear);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    S.drawSum += Ms(t0.QuadPart, t1.QuadPart);
}

// ---- events and commands
void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_FRAME: OnFrame(ev->dt); break;
    case MML_EVENT_MISSION_CLOSE: {   // (the world goes: everything cleared, the region forgotten)
        gpux::Op op{};
        op.kind = gpux::kOpClear;
        Send(op);
        g_haveRegion = false;
        break;
    }
    default: break;
    }
}

void Reply(MmlReply *r, const json &j) {
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    H->reply_append(r, s.c_str(), s.size());
}
int Cmd(uint32_t kind, const char *args, MmlReply *r) {
    json a = json::parse(args ? args : "{}", nullptr, false);
    if (!a.is_object()) a = json::object();
    std::string err;
    try {
        err = Ask(kind, a);
    } catch (const json::exception &e) {
        err = e.what();
    }
    if (!err.empty()) { H->reply_error(r, err.c_str()); return 1; }
    const gpux::Ops &o = *g_ops;
    Reply(r, {{"op", g_serial}, {"region", g_region}, {"statics", o.statics}, {"lo", {o.lo[0], o.lo[1], o.lo[2]}}, {"hi", {o.hi[0], o.hi[1], o.hi[2]}}});
    return 0;
}
int CmdPour(const char *a, MmlReply *r, void *) { return Cmd(gpux::kOpPour, a, r); }
int CmdCloth(const char *a, MmlReply *r, void *) { return Cmd(gpux::kOpCloth, a, r); }
int CmdJelly(const char *a, MmlReply *r, void *) { return Cmd(gpux::kOpJelly, a, r); }
int CmdClear(const char *, MmlReply *r, void *) {
    gpux::Op op{};
    op.kind = gpux::kOpClear;
    Send(op);
    Reply(r, {{"op", g_serial}});
    return 0;
}
int CmdState(const char *, MmlReply *r, void *) {
    uint64_t seen = S.late0 + S.late1 + S.late2;
    double k = seen ? 1.0 / seen : 0;
    std::string err(g_out.error, strnlen(g_out.error, sizeof g_out.error));
    Reply(r, {{"frames", S.frames}, {"late", {{"0", S.late0}, {"1", S.late1}, {"2+", S.late2}}}, {"none", S.none},
              {"round_trip_ms", {{"mean", S.rtSum * k}, {"max", S.rtMax}}}, {"ask_to_done_ms", S.workSum * k},
              {"step_ms", S.stepSum * k}, {"wait_budget_ms", g_waitMs},
              {"waited", {{"frames", S.waited}, {"mean_ms", S.waited ? S.waitSum / S.waited : 0.0}, {"max_ms", S.waitMax}}}, {"copy_ms", S.copySum * k}, {"draw_cpu_ms", S.drawSum * k},
              {"fluid", g_out.fluid}, {"cloth", g_out.cloth}, {"jelly", g_out.jelly}, {"substeps", g_out.substeps},
              {"origin", {g_out.origin[0], g_out.origin[1], g_out.origin[2]}}, {"ops_asked", g_serial}, {"ops_done", g_out.lastOp},
              {"region", g_region}, {"statics", g_ops->statics}, {"error", err}});
    return 0;
}
int CmdWait(const char *args, MmlReply *r, void *) {
    json a = json::parse(args ? args : "{}", nullptr, false);
    if (a.is_object() && a.contains("ms") && a["ms"].is_number()) g_waitMs = std::min(a["ms"].get<uint32_t>(), 30u);
    Reply(r, {{"wait_ms", g_waitMs}});
    return 0;
}
int CmdReset(const char *, MmlReply *r, void *) {
    S = Stats{};
    Reply(r, {{"reset", true}});
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, channel_wait) || !MML_HAS(H, world_shapes)) return 1;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_opsCh = H->channel_open(mod, "ops", sizeof(gpux::Ops));
    g_frameCh = H->channel_open(mod, "frame", sizeof(gpux::Frame));
    g_outCh = H->channel_open(mod, "out", sizeof(gpux::Out));
    g_topoCh = H->channel_open(mod, "topology", sizeof(gpux::Topology));
    if (!g_opsCh || !g_frameCh || !g_outCh || !g_topoCh) return 1;
    g_shapes.resize(gpux::kMaxStatics);
    // (a reload: the 64-bit half is new too, its scene empty: the ops' numbers start again with the channel's)
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_MISSION_CLOSE}) H->subscribe(mod, t, OnEvent, nullptr);
    if (!H->gfx_pass(mod, 500, Pass, nullptr)) return 1;
    H->command(mod, "pour", "{count, ahead, height, spacing} -> a block of water (PBD particles on the GPU) in front of the player", CmdPour, nullptr);
    H->command(mod, "cloth", "{count, size, ahead, height} -> a sheet of cloth (FEM on the GPU) dropped from above", CmdCloth, nullptr);
    H->command(mod, "jelly", "{size, ahead, height, stiffness} -> a soft cube (an FEM soft body on the GPU) dropped in front", CmdJelly, nullptr);
    H->command(mod, "clear", "{} -> the water, the cloth and the jelly taken away", CmdClear, nullptr);
    H->command(mod, "state", "{} -> what this half saw: frames, how late the points were, the round trip, the GPU's step", CmdState, nullptr);
    H->command(mod, "reset", "{} -> the measures started again", CmdReset, nullptr);
    H->command(mod, "wait", "{ms} -> how long the pass waits for this frame's step (0: never, the latest one drawn; at most 30)", CmdWait, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) { G.Free(); }
