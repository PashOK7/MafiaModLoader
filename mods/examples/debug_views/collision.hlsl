// debug_views, collision: the pass (collision.cpp). Two steps a frame:
// 1. VsCollision + PsCollisionG: the collision's triangles (the static world's and the moving objects', flat-shaded:
//    each vertex carries its face's normal) drawn into the mod's own targets with a depth buffer of its own, so every
//    pixel keeps the nearest collision surface alone: its colour (t3) and its world normal with its linear view depth
//    (t4). No order, no blending: a far face never shows through a near one.
// 2. PsCollision (a full-screen pass into the effects target): that nearest surface laid over the frame where it is
//    not behind what the game drew. Within `g_tol` of the scene's depth the collision wins (it lies on the world's own
//    surfaces: no fighting), farther behind it is hidden (a wall really nearer is drawn); `through` shows it whatever
//    the scene. Shaded by its facing to a light, lined by a world grid (triplanar: a metre a cell, a bolder line every
//    five), darkened by an ambient occlusion read from its own depth and normals
//    (so the shapes' volume reads), outlined where the surface's normal or depth jumps (the shapes' edges; the seams
//    between two faces of one plane are not edges).
#include "mml/gfx/frame.hlsli"

cbuffer Collision : register(b1) {
    row_major float4x4 g_viewProj;   // the world to the frame's clip space
    float4 g_style;                  // x: the fill's opacity, y: 1 outlines, z: the occlusion's strength, w: 1 through
    float4 g_tol;                    // x: metres the collision wins by, y: and this share of the depth
    float4 g_grid;                   // x: the world grid's cell (metres, 0 none), y: a major line every this many cells
};

Texture2D<float4> g_collColor : register(t3);   // rgb the surface's colour, a 1 where there is collision
Texture2D<float4> g_collNormal : register(t4);  // xyz its world normal, w its linear view depth (metres)

struct VsIn { float3 pos : POSITION; float3 normal : NORMAL; float4 color : COLOR; };
struct VsOut { float4 pos : SV_Position; float3 world : TEXCOORD0; float3 normal : TEXCOORD1; float4 color : COLOR; };

VsOut VsCollision(VsIn v) {
    VsOut o;
    o.pos = mul(float4(v.pos, 1), g_viewProj);
    o.world = v.pos;
    o.normal = v.normal;
    o.color = v.color;
    return o;
}

struct GOut { float4 color : SV_Target0; float4 normal : SV_Target1; };
GOut PsCollisionG(VsOut i) {
    GOut o;
    o.color = float4(i.color.rgb, 1);
    o.normal = float4(normalize(i.normal), dot(i.world - g_eye.xyz, g_fwd.xyz));
    return o;
}

// The world point the collision shows at a pixel (its own depth).
float3 CollPoint(int2 p, float z) { return g_eye.xyz + ViewRay(float2(p) + 0.5) * z; }

// The point where the ray through a pixel meets the plane through P with the normal n (the surface's own plane: its
// change from one pixel to the next is the surface's, also at a shape's edge, where the next pixel shows another).
float3 OnPlane(float2 pixel, float3 P, float3 n) {
    const float3 r = ViewRay(pixel);
    const float t = dot(P - g_eye.xyz, n) / (abs(dot(r, n)) > 1e-6 ? dot(r, n) : 1e-6);
    return g_eye.xyz + r * t;
}
// A grid's lines along one coordinate pair: 1 on a line (about a pixel and a half wide whatever the distance,
// antialiased by the coordinates' change between neighbouring pixels, dx and dy), 0 between them; gone where the
// cells shrink under a few pixels (no moire far away).
float GridLines(float2 c, float2 dx, float2 dy, float cell) {
    const float2 u = c / cell;
    const float2 w = max((abs(dx) + abs(dy)) / cell, 1e-5);
    const float2 d = abs(frac(u - 0.5) - 0.5) / (w * 1.5);   // the distance to the nearest line, in line widths
    const float on = 1 - saturate(min(d.x, d.y));
    return on * saturate(1.5 - max(w.x, w.y) * 6);       // (cells under about 4 pixels: no lines)
}
// The world grid on a surface (triplanar: each of the three planes weighted by how much the surface faces its axis, so
// a wall has the upright grid, the ground the level one, a slope a blend): x the minor lines, y the major ones.
// dx, dy: the surface point's change to the next pixel across and down.
float2 WorldGrid(float3 P, float3 dx, float3 dy, float3 n) {
    if (g_grid.x <= 0) return 0;
    float3 w = pow(abs(n), 4);
    w /= w.x + w.y + w.z;
    float2 lines = 0;
    [unroll] for (int k = 0; k < 2; k++) {
        const float cell = k == 0 ? g_grid.x : g_grid.x * max(g_grid.y, 1);
        lines[k] = GridLines(P.yz, dx.yz, dy.yz, cell) * w.x + GridLines(P.xz, dx.xz, dy.xz, cell) * w.y +
                   GridLines(P.xy, dx.xy, dy.xy, cell) * w.z;
    }
    return lines;
}

float4 PsCollision(float4 pos : SV_Position) : SV_Target {
    const int2 p = int2(pos.xy);
    const float4 c = g_collColor.Load(int3(p, 0));
    if (c.a < 0.5) return 0;                       // no collision at this pixel
    const float4 nd = g_collNormal.Load(int3(p, 0));
    const float z = nd.w;
    if (g_style.w < 0.5) {
        const float scene = SceneDepth(pos.xy);
        if (!IsSky(scene) && z > scene + g_tol.x + scene * g_tol.y) return 0;   // the world is really in front
    }
    float3 n = normalize(nd.xyz);
    const float3 eye = normalize(g_eye.xyz - CollPoint(p, z));
    if (dot(n, eye) < 0) n = -n;                   // (a face seen from behind: lit as its front)

    // the light: a key from above and to the side, a little from the eye, so each side of a solid reads apart
    const float3 key = normalize(float3(0.35, 0.85, 0.4));
    float shade = 0.4 + 0.45 * saturate(dot(n, key)) + 0.15 * saturate(dot(n, eye));

    // ambient occlusion from the collision's own surfaces: points around this one (a disc of 0.7 m on the screen)
    // above its plane and near it shade it
    const float radius = 0.7;
    const float px = radius * g_proj.y * 0.5 * g_size.y / max(z, 0.1);   // the disc's radius in pixels
    const float3 P = CollPoint(p, z);
    float occluded = 0;
    const int taps = 12;
    [unroll] for (int k = 0; k < taps; k++) {
        const float a = (k + 0.5) * 2.39996323;     // (the golden angle: the taps spread over the disc)
        const float r = sqrt((k + 0.5) / taps) * px;
        const int2 q = clamp(p + int2(cos(a) * r, sin(a) * r), int2(0, 0), int2(g_size.xy) - 1);
        const float4 qnd = g_collNormal.Load(int3(q, 0));
        if (g_collColor.Load(int3(q, 0)).a < 0.5) continue;
        const float3 v = CollPoint(q, qnd.w) - P;
        const float d = length(v);
        if (d < 1e-3) continue;
        occluded += saturate(dot(n, v / d) - 0.1) * (1 - saturate(d / radius));
    }
    const float ao = 1 - g_style.z * saturate(occluded / taps * 2.5);

    // outlines: where the normal turns, the neighbour's point leaves this pixel's plane (a step, a solid in front: the
    // distance to the plane, not the depth, so a ground seen at a grazing angle has none), the surface changes, or the
    // collision ends
    float edge = 0;
    if (g_style.y > 0.5) {
        const int2 nb[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
        [unroll] for (int e = 0; e < 4; e++) {
            const int2 q = clamp(p + nb[e], int2(0, 0), int2(g_size.xy) - 1);
            const float4 qc = g_collColor.Load(int3(q, 0));
            const float4 qnd = g_collNormal.Load(int3(q, 0));
            const bool gone = qc.a < 0.5;
            const bool turns = abs(dot(normalize(qnd.xyz), normalize(nd.xyz))) < 0.94;
            const bool steps = abs(dot(CollPoint(q, qnd.w) - P, n)) > 0.03 + z * 0.002;
            const bool other = any(abs(qc.rgb - c.rgb) > 0.02);
            if (gone || turns || steps || other) edge = 1;
        }
    }
    // the world grid: the surface's relief, its slope and its scale read from the cells (a metre by default)
    const float2 at = float2(p) + 0.5;
    const float2 grid = WorldGrid(P, OnPlane(at + float2(1, 0), P, n) - P, OnPlane(at + float2(0, 1), P, n) - P, n);
    shade *= 1 - 0.18 * grid.x - 0.22 * grid.y;
    float3 rgb = c.rgb * shade * ao;
    float alpha = g_style.x;
    if (edge > 0) { rgb *= 0.35; alpha = max(alpha, 0.85); }
    return float4(rgb * alpha, alpha);
}
