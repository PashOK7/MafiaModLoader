// debug_views, collision: the world's collision drawn where it is, as solid shapes, each surface its colour (a debug
// view for mission and mod authors; after pet1kk's developer tools, PR 2, which first drew it all).
//
// What is drawn, within `range` of the camera: every static shape (the triangles, the boxes, the spheres, the poles as
// upright cylinders from the ground under them up 6 m), coloured by its surface (the material: MmlShape's flags >> 24;
// its name the game's own, tables\materials.def, material_name), and the moving objects' (`moving`: the cars' hulls
// white, the rest grey; `people`: a person's ball, a traffic pedestrian's box, pale yellow), read every frame.
//
// How (a graphics pass, collision.hlsl): the shapes' triangles are drawn into the mod's own targets with a depth buffer
// of its own, so each pixel keeps the nearest collision surface alone (no order, no blending: a far face never shows
// through a near one); then that surface is laid over the frame, translucent (`fill`), lit by its facing, darkened by
// an ambient occlusion read from its own depth and normals (`ao`), a world grid on it (`grid` metres a cell, a bolder
// line every 5: the relief, the slopes and the scale read from it; triplanar, each face the grid of the plane it
// faces most), outlined where its normal turns, it steps off its
// plane or its surface changes (`edges`; the seams between the faces of one plane are no edges, and no face is moved:
// the ground has no gaps). Against the game's drawing: within `tolerance` metres (and a little more far away) of what
// the frame shows the collision wins, so it lies on the world's own surfaces without fighting them; what the game
// draws farther in front of it (Tommy, a car, a wall that really is nearer) hides it. `through`: shown whatever stands
// in front. It needs the graphics bridge (the mod on when the game starts: "gfx": true).
//
// Labels: under the crosshair, what the camera looks at (a surface, its number, name and distance, or a moving
// object); around it, a label for each surface near (one a surface every few metres).
//
// The static shapes come from world_shapes (the game's own grid), read again when the camera has moved 5 m or every
// 2 s. Its key (Shift+F9) shows and hides it; Options > Mods > Debug views > Collision has the rest. Command:
// debug_views.collision.state.
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "shaders/PsCollision.h"
#include "shaders/PsCollisionG.h"
#include "shaders/VsCollision.h"
#include "lines.h"
#include "views.h"

#define SECTION "collision."

namespace {
const MmlHost *H;
MmlMod *M;
// The mission's world is there to draw over (in play, or under the pause menu: the menus are drawn over the views).
bool Shown() { const int st = H->mission_state(); return st == MML_MISSION_RUNNING || st == MML_MISSION_PAUSED; }
bool g_on;

struct P3 { float x, y, z; };
P3 operator+(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
P3 operator-(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P3 operator*(P3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
P3 Cross(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(P3 a) { return std::sqrt(Dot(a, a)); }
P3 Norm(P3 a) { float l = Len(a); return l > 1e-6f ? a * (1 / l) : a; }
P3 At(const float *f) { return {f[0], f[1], f[2]}; }

// A vertex as the pass reads it: its place, its face's normal (flat shading), its colour (R, G, B, A bytes).
struct Vertex { float x, y, z, nx, ny, nz; uint32_t rgba; };
std::vector<Vertex> g_static;          // the static world's triangles, made at each read
std::vector<Vertex> g_dynamic;         // the moving objects', made every frame
std::vector<Vertex> *g_to = &g_static; // where the shapes being made go
bool g_staticDirty = true;             // made anew: to be uploaded
std::vector<MmlShape> g_shapes;        // the last read
struct Mark { P3 at; uint32_t material; };
std::vector<Mark> g_marks;             // a place of each surface near the camera (its label)
std::map<uint32_t, int> g_counts;      // the triangles by surface
int g_total, g_poles, g_moving;
P3 g_readAt{1e9f, 0, 0};
DWORD g_readWhen;
uint64_t g_passes;                     // frames the pass drew

uint32_t Rgba(uint32_t argb) { return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16); }

std::string MaterialText(uint32_t m) {
    const char *n = H->material_name(static_cast<int>(m));
    char b[96];
    snprintf(b, sizeof b, "surface %u%s%s", m, n ? ": " : "", n ? n : "");
    return b;
}

// The ground under a pole: the static world straight down from above the camera (the pole stands from far below).
float GroundUnder(float x, float z, float from) {
    float a[3] = {x, from + 30, z}, b[3] = {x, from - 100, z};
    MmlHit hit{};
    hit.size = sizeof hit;
    return H->world_ray(M, a, b, MML_WORLD_STATIC, nullptr, &hit) ? hit.pos[1] : from - 2;
}

// ---- the shapes as triangles (each vertex its face's normal; the shading is the pass's)
void Tri(const P3 &a, const P3 &b, const P3 &c, P3 n, uint32_t argb) {
    n = Norm(n);
    const uint32_t col = Rgba(argb);
    for (const P3 &p : {a, b, c}) g_to->push_back({p.x, p.y, p.z, n.x, n.y, n.z, col});
}
// A box from its centre, half sizes and axes: six sides.
void Box(const MmlShape &s, uint32_t argb) {
    const P3 c = At(s.center), ax[3] = {At(s.axes), At(s.axes + 3), At(s.axes + 6)};
    P3 k[8];
    for (int j = 0; j < 8; j++)
        k[j] = c + ax[0] * (s.half[0] * ((j & 1) ? 1 : -1)) + ax[1] * (s.half[1] * ((j & 2) ? 1 : -1)) + ax[2] * (s.half[2] * ((j & 4) ? 1 : -1));
    static const int side[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
    const P3 normal[6] = {ax[0] * -1, ax[0], ax[1] * -1, ax[1], ax[2] * -1, ax[2]};
    for (int f = 0; f < 6; f++) {
        const int *q = side[f];
        Tri(k[q[0]], k[q[1]], k[q[2]], normal[f], argb);
        Tri(k[q[0]], k[q[2]], k[q[3]], normal[f], argb);
    }
}
// A sphere: 12 rings of 24 pieces, each face its own normal.
void Sphere(const P3 &c, float r, uint32_t argb) {
    const int rings = 12, segs = 24;
    auto at = [&](int i, int j) {
        const float t = 3.1415927f * i / rings, u = 6.2831853f * j / segs;
        return P3{std::sin(t) * std::cos(u), std::cos(t), std::sin(t) * std::sin(u)};
    };
    for (int i = 0; i < rings; i++)
        for (int j = 0; j < segs; j++) {
            const P3 n00 = at(i, j), n01 = at(i, j + 1), n10 = at(i + 1, j), n11 = at(i + 1, j + 1), n = n00 + n01 + n10 + n11;
            Tri(c + n00 * r, c + n10 * r, c + n11 * r, n, argb);
            Tri(c + n00 * r, c + n11 * r, c + n01 * r, n, argb);
        }
}
// A pole: an upright cylinder of 24 sides from y0 up `tall` metres, its top closed.
void Pole(float x, float z, float r, float y0, float tall, uint32_t argb) {
    const int segs = 24;
    for (int j = 0; j < segs; j++) {
        const float u0 = 6.2831853f * j / segs, u1 = 6.2831853f * (j + 1) / segs, um = (u0 + u1) / 2;
        const P3 a{x + std::cos(u0) * r, y0, z + std::sin(u0) * r}, b{x + std::cos(u1) * r, y0, z + std::sin(u1) * r};
        const P3 a2 = a + P3{0, tall, 0}, b2 = b + P3{0, tall, 0}, side{std::cos(um), 0, std::sin(um)};
        Tri(a, b, b2, side, argb);
        Tri(a, b2, a2, side, argb);
        Tri(a2, b2, P3{x, y0 + tall, z}, P3{0, 1, 0}, argb);
    }
}
void Solid(const MmlShape &s, uint32_t argb) {
    if (s.kind == MML_SHAPE_BOX) Box(s, argb);
    else if (s.kind == MML_SHAPE_SPHERE) Sphere(At(s.center), s.half[0], argb);
}

// The moving objects' colours, none of the surfaces' palette (lines::Palette): a car's hull white, the other moving
// objects grey, people pale yellow.
constexpr uint32_t kCarColour = 0xFFFFFFFFu, kObjectColour = 0xFF808080u, kPersonColour = 0xFFFFE8A0u;

// The moving objects within `range` of the eye, read every frame. The game keeps two shapes for each (seen in both
// builds; physx_crates reads them so too): a car's body over its wheels (classes 0x201) and its whole hull down to the
// ground (0x202), a person's sphere of 0.3 m at his model's place (0x101) and a wide one of 1.2 m at its `base` frame
// (0x102: the aim's broad phase: the player's aim ray meets it, then the person's own model is tested inside it;
// knowledge/notes/game.md "A person's two spheres"). The traffic's pedestrians are no actors of their own: each is an
// upright box of class 0x100 alone (about 0.95 x 1.7 x 0.95 m), its owner the pedestrians' manager. Drawn: a car's
// hull (its body lies inside it), a person's narrow ball, a pedestrian's box, the other moving objects (class 0x800 and
// the rest); not drawn: a person's wide sphere (nothing meets it as a solid). A person has no box or capsule: his own
// moves sweep three spheres from his feet up against the world (knowledge/notes/game.md "A person's step"), and
// bullets test his bones.
void ReadMoving(const P3 &eye) {
    g_dynamic.clear();
    g_moving = 0;
    const bool moving = H->config_number(M, SECTION "moving", 1) != 0, people = H->config_number(M, SECTION "people", 1) != 0;
    if (!moving && !people) return;
    const float range = static_cast<float>(H->config_number(M, SECTION "range", 40));
    const float lo[3] = {eye.x - range, eye.y - range, eye.z - range}, hi[3] = {eye.x + range, eye.y + range, eye.z + range};
    static std::vector<MmlShape> shapes(1024);
    int n = H->world_shapes(M, lo, hi, MML_WORLD_MOVING, shapes.data(), static_cast<int>(shapes.size()));
    n = (std::max)(0, (std::min)(n, static_cast<int>(shapes.size())));
    g_to = &g_dynamic;
    for (int i = 0; i < n; i++) {
        const MmlShape &s = shapes[i];
        const bool person = (s.classes & 0x100) != 0, car = (s.classes & 0x200) != 0;
        if (person ? !people : !moving) continue;
        if ((person && (s.classes & 0x2)) || (car && (s.classes & 0x1))) continue;   // (a wide sphere; a body in its hull)
        Solid(s, person ? kPersonColour : car ? kCarColour : kObjectColour);
        g_moving++;
    }
    g_to = &g_static;
}

void Read(const P3 &eye) {
    g_readAt = eye;
    g_readWhen = GetTickCount();
    const float range = static_cast<float>(H->config_number(M, SECTION "range", 40));
    const float lo[3] = {eye.x - range, eye.y - range, eye.z - range}, hi[3] = {eye.x + range, eye.y + range, eye.z + range};
    if (g_shapes.size() < 8192) g_shapes.resize(8192);
    int n = H->world_shapes(M, lo, hi, MML_WORLD_STATIC, g_shapes.data(), static_cast<int>(g_shapes.size()));
    if (n > static_cast<int>(g_shapes.size()) && n < 200000) {   // (more than the buffer: read again whole)
        g_shapes.resize(static_cast<size_t>(n));
        n = H->world_shapes(M, lo, hi, MML_WORLD_STATIC, g_shapes.data(), static_cast<int>(g_shapes.size()));
    }
    n = (std::max)(0, (std::min)(n, static_cast<int>(g_shapes.size())));
    g_total = n;
    g_static.clear();
    g_marks.clear();
    g_counts.clear();
    g_poles = 0;
    g_to = &g_static;
    const float labelRange = static_cast<float>(H->config_number(M, SECTION "label_range", 15));
    for (int i = 0; i < n; i++) {
        const MmlShape &s = g_shapes[i];
        const uint32_t m = s.flags >> 24, col = lines::Palette(m);
        P3 c = At(s.center);
        if (s.kind == MML_SHAPE_TRIANGLE) {
            g_counts[m]++;
            const P3 a = At(s.corner[0]), b = At(s.corner[1]), d = At(s.corner[2]);
            P3 nrm = At(s.normal);
            if (Len(nrm) < 0.5f) nrm = Cross(b - a, d - a);
            Tri(a, b, d, nrm, col);
            c = (a + b + d) * (1.0f / 3);
        } else if (s.kind == MML_SHAPE_BOX || s.kind == MML_SHAPE_SPHERE) {
            Solid(s, col);
        } else if (s.kind == MML_SHAPE_POLE) {
            if (++g_poles > 256) continue;   // (each asks the ground once)
            const float y = GroundUnder(s.center[0], s.center[2], eye.y);
            Pole(s.center[0], s.center[2], s.half[0], y, 6.0f, col);
            c = {s.center[0], y + 2, s.center[2]};
        }
        if (Len(c - eye) > labelRange || g_marks.size() >= 48) continue;
        bool close = false;   // (one label a surface every 6 m)
        for (const Mark &k : g_marks) close = close || (k.material == m && Len(k.at - c) < 6);
        if (!close) g_marks.push_back({c, m});
    }
    g_staticDirty = true;
}

bool Eye(P3 *eye, P3 *dir) {
    float o[3], d[3];
    if (!H->camera_ray(M, 400, 300, o, d)) return false;
    *eye = At(o);
    if (dir) *dir = At(d);
    return true;
}

constexpr int kCrosshairPlace = 1;   // (its line's place among the views' labels: lines.h Reserve)
void OnFrame(const MmlEvent *, void *) {
    if (!g_on || !Shown()) { lines::Unreserve(kCrosshairPlace); return; }
    P3 eye, dir;
    if (!Eye(&eye, &dir)) { lines::Unreserve(kCrosshairPlace); return; }
    if (Len(eye - g_readAt) > 5 || GetTickCount() - g_readWhen > 2000) Read(eye);
    ReadMoving(eye);
    if (H->config_number(M, SECTION "labels", 1) != 0)
        for (const Mark &k : g_marks) { float at[3] = {k.at.x, k.at.y + 0.3f, k.at.z}; lines::Label(H, M, at, {MaterialText(k.material)}, lines::Palette(k.material)); }
    // what the crosshair looks at (the static world, or a moving object: not the player), a place every view's labels
    // go round (lines.h Reserve)
    float a[3] = {eye.x, eye.y, eye.z}, b[3] = {eye.x + dir.x * 200, eye.y + dir.y * 200, eye.z + dir.z * 200};
    MmlHit hit{};
    hit.size = sizeof hit;
    if (!H->world_ray(M, a, b, MML_WORLD_ALL, H->human_player(M), &hit)) { lines::Unreserve(kCrosshairPlace); return; }
    char line[160];
    const uint32_t kind = hit.flags & 0xFF;
    if (hit.actor)
        snprintf(line, sizeof line, "%s, %.1f m (a moving %s)", (hit.classes & 0x200) ? "a car" : (hit.classes & 0x100) ? "a person" : "an object",
                 hit.fraction * 200, kind == 0x40 ? "box" : "sphere");
    else
        snprintf(line, sizeof line, "%s, %.1f m (a %s)", MaterialText(hit.flags >> 24).c_str(), hit.fraction * 200,
                 kind <= 7 ? "triangle" : kind == 0x82 ? "sphere" : kind == 0x84 ? "pole" : "box");
    const float w = H->draw_text_width(MML_FONT_SMALL, 13, line);
    lines::Reserve(kCrosshairPlace, 400 - w / 2 - 5, 318, 400 + w / 2 + 5, 336);
    H->draw_rect(M, 400 - w / 2 - 5, 318, w + 10, 18, 0xA0000000u, 1);
    H->draw_text(M, 400, 320, 13, hit.actor ? ((hit.classes & 0x200) ? kCarColour : (hit.classes & 0x100) ? kPersonColour : kObjectColour) : lines::Palette(hit.flags >> 24),
                 MML_FONT_SMALL, MML_DRAW_CENTER, line);
}

// ---- the pass (Direct3D 11, the graphics bridge's device)
template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_psG, *g_psC;
ID3D11InputLayout *g_layout;
ID3D11Buffer *g_cb, *g_vbStatic, *g_vbDynamic;
UINT g_vbStaticCount, g_vbDynamicCap;
ID3D11RasterizerState *g_rs;
ID3D11DepthStencilState *g_dss;
ID3D11Texture2D *g_colorTex, *g_normalTex, *g_depthTex;
ID3D11RenderTargetView *g_colorRtv, *g_normalRtv;
ID3D11ShaderResourceView *g_colorSrv, *g_normalSrv;
ID3D11DepthStencilView *g_dsv;
unsigned g_madeFor, g_w, g_h;

// cbuffer Collision (b1), its layout in collision.hlsl
struct CollisionCb { float viewProj[16]; float style[4]; float tol[4]; float grid[4]; };

void ReleaseTargets() {
    SafeRelease(g_colorSrv); SafeRelease(g_normalSrv); SafeRelease(g_colorRtv); SafeRelease(g_normalRtv); SafeRelease(g_dsv);
    SafeRelease(g_colorTex); SafeRelease(g_normalTex); SafeRelease(g_depthTex);
    g_w = g_h = 0;
}
void ReleaseAll() {
    ReleaseTargets();
    SafeRelease(g_vs); SafeRelease(g_psG); SafeRelease(g_psC); SafeRelease(g_layout); SafeRelease(g_cb);
    SafeRelease(g_vbStatic); SafeRelease(g_vbDynamic); SafeRelease(g_rs); SafeRelease(g_dss);
    g_vbStaticCount = g_vbDynamicCap = 0;
    g_staticDirty = true;
}

bool MakeShaders(ID3D11Device *d) {
    ReleaseAll();
    if (FAILED(d->CreateVertexShader(kVsCollision, sizeof kVsCollision, nullptr, &g_vs)) ||
        FAILED(d->CreatePixelShader(kPsCollisionG, sizeof kPsCollisionG, nullptr, &g_psG)) ||
        FAILED(d->CreatePixelShader(kPsCollision, sizeof kPsCollision, nullptr, &g_psC)))
        return false;
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(d->CreateInputLayout(layout, 3, kVsCollision, sizeof kVsCollision, &g_layout))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CollisionCb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(d->CreateBuffer(&bd, nullptr, &g_cb))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;   // (a box seen from inside, a triangle from behind: drawn all the same)
    rd.DepthClipEnable = TRUE;
    if (FAILED(d->CreateRasterizerState(&rd, &g_rs))) return false;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    return SUCCEEDED(d->CreateDepthStencilState(&dd, &g_dss));
}

bool MakeTargets(ID3D11Device *d, unsigned w, unsigned h) {
    ReleaseTargets();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &g_colorTex))) return false;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;   // (the normal and the depth: the depth in metres wants 32 bits)
    if (FAILED(d->CreateTexture2D(&td, nullptr, &g_normalTex))) return false;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &g_depthTex))) return false;
    if (FAILED(d->CreateRenderTargetView(g_colorTex, nullptr, &g_colorRtv)) || FAILED(d->CreateRenderTargetView(g_normalTex, nullptr, &g_normalRtv)) ||
        FAILED(d->CreateShaderResourceView(g_colorTex, nullptr, &g_colorSrv)) || FAILED(d->CreateShaderResourceView(g_normalTex, nullptr, &g_normalSrv)) ||
        FAILED(d->CreateDepthStencilView(g_depthTex, nullptr, &g_dsv)))
        return false;
    g_w = w;
    g_h = h;
    return true;
}

// The static triangles into a buffer of their own (made anew at each read); the moving ones into a buffer written
// each frame.
bool Upload(ID3D11Device *d, ID3D11DeviceContext *ctx) {
    if (g_staticDirty) {
        SafeRelease(g_vbStatic);
        g_vbStaticCount = static_cast<UINT>(g_static.size());
        if (g_vbStaticCount) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = g_vbStaticCount * sizeof(Vertex);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA sd{g_static.data(), 0, 0};
            if (FAILED(d->CreateBuffer(&bd, &sd, &g_vbStatic))) { g_vbStaticCount = 0; return false; }
        }
        g_staticDirty = false;
    }
    if (g_dynamic.empty()) return true;
    if (g_dynamic.size() > g_vbDynamicCap) {
        SafeRelease(g_vbDynamic);
        g_vbDynamicCap = static_cast<UINT>(g_dynamic.size() * 2);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = g_vbDynamicCap * sizeof(Vertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d->CreateBuffer(&bd, nullptr, &g_vbDynamic))) { g_vbDynamicCap = 0; return false; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_vbDynamic, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, g_dynamic.data(), g_dynamic.size() * sizeof(Vertex));
    ctx->Unmap(g_vbDynamic, 0);
    return true;
}

void Pass(const MmlGfxFrame *f, void *) {
    if (!g_on || !Shown() || (g_static.empty() && g_dynamic.empty()) || !f->width || !f->height) return;
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_vs) {
        if (!MakeShaders(d)) { ReleaseAll(); return; }
        g_madeFor = f->generation;
    }
    if ((g_w != f->width || g_h != f->height) && !MakeTargets(d, f->width, f->height)) { ReleaseTargets(); return; }
    if (!Upload(d, ctx)) return;
    g_passes++;

    CollisionCb cb{};
    for (int r = 0; r < 4; r++)   // view * proj (row vectors: a point times view, then proj)
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 4; k++) cb.viewProj[r * 4 + c] += f->view[r * 4 + k] * f->proj[k * 4 + c];
    cb.style[0] = static_cast<float>((std::max)(0.0, (std::min)(1.0, H->config_number(M, SECTION "fill", 0.6))));
    cb.style[1] = H->config_number(M, SECTION "edges", 1) != 0 ? 1.0f : 0.0f;
    cb.style[2] = static_cast<float>((std::max)(0.0, (std::min)(1.0, H->config_number(M, SECTION "ao", 0.7))));
    cb.style[3] = H->config_number(M, SECTION "through", 0) != 0 ? 1.0f : 0.0f;
    cb.tol[0] = static_cast<float>(H->config_number(M, SECTION "tolerance", 0.15));
    cb.tol[1] = 0.004f;   // (and 0.4 % of the depth: the depth buffer's steps grow far away)
    cb.grid[0] = static_cast<float>((std::max)(0.0, H->config_number(M, SECTION "grid", 1)));
    cb.grid[1] = 5;       // (a major line every 5 cells)
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);

    // what the bridge bound for the pass, put back for the second step
    ID3D11RenderTargetView *target = nullptr;
    ID3D11DepthStencilView *targetDepth = nullptr;
    ctx->OMGetRenderTargets(1, &target, &targetDepth);
    ID3D11BlendState *blend = nullptr;
    float factor[4];
    UINT mask = 0;
    ctx->OMGetBlendState(&blend, factor, &mask);
    D3D11_VIEWPORT viewport{};
    UINT viewports = 1;
    ctx->RSGetViewports(&viewports, &viewport);

    // 1. the nearest collision surface of each pixel, into the mod's own targets
    const float none[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g_colorRtv, none);
    ctx->ClearRenderTargetView(g_normalRtv, none);
    ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11ShaderResourceView *unbound[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(3, 2, unbound);
    ID3D11RenderTargetView *rts[2] = {g_colorRtv, g_normalRtv};
    ctx->OMSetRenderTargets(2, rts, g_dsv);
    D3D11_VIEWPORT full{0, 0, static_cast<float>(f->width), static_cast<float>(f->height), 0, 1};
    ctx->RSSetViewports(1, &full);
    ctx->RSSetState(g_rs);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g_dss, 0);
    ctx->IASetInputLayout(g_layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->VSSetConstantBuffers(1, 1, &g_cb);
    ctx->PSSetShader(g_psG, nullptr, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_cb);
    const UINT stride = sizeof(Vertex), offset = 0;
    if (g_vbStatic && g_vbStaticCount) {
        ctx->IASetVertexBuffers(0, 1, &g_vbStatic, &stride, &offset);
        ctx->Draw(g_vbStaticCount, 0);
    }
    if (g_vbDynamic && !g_dynamic.empty()) {
        ctx->IASetVertexBuffers(0, 1, &g_vbDynamic, &stride, &offset);
        ctx->Draw(static_cast<UINT>(g_dynamic.size()), 0);
    }

    // 2. laid over the frame (the effects target, the bridge's blending), against the scene's depth
    ctx->OMSetRenderTargets(1, &target, targetDepth);
    ctx->OMSetBlendState(blend, factor, mask);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(static_cast<ID3D11VertexShader *>(f->full_screen), nullptr, 0);
    ID3D11ShaderResourceView *srvs[2] = {g_colorSrv, g_normalSrv};
    ctx->PSSetShaderResources(3, 2, srvs);
    ctx->PSSetShader(g_psC, nullptr, 0);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(3, 2, unbound);   // (written again next frame)
    SafeRelease(target);
    SafeRelease(targetDepth);
    SafeRelease(blend);
}

void ReadSettings() {
    g_on = H->config_number(M, SECTION "show", 0) != 0;
    g_readWhen = 0;   // (read again: the range may have changed)
    g_readAt = {1e9f, 0, 0};
    H->hotkey(M, H->config_string(M, SECTION "hotkey", "Shift+F9"), [](void *) { g_on = !g_on; H->config_set(M, SECTION "show", g_on ? "true" : "false"); }, nullptr);
}

// debug_views.collision.state: what was read, by surface
int State(const char *, MmlReply *reply, void *) {
    std::string out = "{\"show\":" + std::string(g_on ? "true" : "false") + ",\"shapes\":" + std::to_string(g_total) + ",\"static_triangles\":" +
                      std::to_string(g_static.size() / 3) + ",\"moving\":" + std::to_string(g_moving) + ",\"moving_triangles\":" +
                      std::to_string(g_dynamic.size() / 3) + ",\"frames_drawn\":" + std::to_string(g_passes) + ",\"surfaces\":[";
    bool first = true;
    for (const auto &c : g_counts) {
        const char *nm = H->material_name(static_cast<int>(c.first));
        std::string name = nm ? nm : "";
        for (char &ch : name) if (ch == '"' || ch == '\\') ch = '\'';
        out += (first ? "" : ",") + std::string("{\"surface\":") + std::to_string(c.first) + ",\"name\":\"" + name + "\",\"triangles\":" + std::to_string(c.second) + "}";
        first = false;
    }
    out += "]}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}
} // namespace

int CollisionLoad(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, material_name)) return 1;   // a loader older than this view
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, [](const MmlEvent *, void *) { ReadSettings(); }, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, [](const MmlEvent *, void *) {
        g_static.clear(); g_dynamic.clear(); g_marks.clear(); g_staticDirty = true; g_readAt = {1e9f, 0, 0};
    }, nullptr);
    if (!MML_HAS(host, gfx_pass) || host->gfx_pass(mod, 105, Pass, nullptr) <= 0)
        host->log(mod, MML_LOG_WARN, "collision: the graphics bridge refused the pass (gfx_state says why): only the labels are drawn");
    host->command(mod, "collision.state", "the collision read around the camera: its shapes, the triangles drawn, the moving objects, the triangles by surface and the surfaces' names", State, nullptr);
    host->log(mod, MML_LOG_INFO, "collision loaded (game %s)", host->game_build());
    return 0;
}

void CollisionUnload() { ReleaseAll(); }
