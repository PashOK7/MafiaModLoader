// physx_gpu's drawing (physx_gpu.cpp's graphics pass, Direct3D 11, shader model 5): what PhysX steps on the GPU in the
// 64-bit half, drawn into the game's frame from the shared texture of points (physx_gpu.h: R32_FLOAT, four texels a
// point, 1024 points a row, about the scene's origin).
//   the cloth and the jelly: their triangles, the vertices read from the points, the normals made by CsNormals;
//   the water: screen-space fluid. Each particle a sphere's front (its linear depth, PsSplatDepth) and its thickness
//   (PsSplatThick); the depth smoothed by a bilateral filter (PsSmooth, two directions); a surface made from it
//   (PsFluid): normals from the smoothed depth, Fresnel's reflection of the sky and the sun, the frame behind refracted
//   and darkened by the thickness, the world's fog.
// Everything is hidden by the world in front of it (the bridge's linear depth) and by the cloth and the jelly (their
// own linear depth, t5).
#include "mml/gfx/frame.hlsli"

cbuffer Gpu : register(b1) {
    float4 g_origin;      // xyz: the points' origin in the world; w: the water particles' radius
    uint4 g_rows;         // x: the first point of what is drawn (a row times 1024), y: points, z: the smoothing's direction
                          // (0 x, 1 y), w: the mesh (0 the cloth, 1 the jelly)
    float4 g_water;       // x: the smoothing's radius in pixels at 1 m, y: its depth falloff, z: the thickness' scale,
                          // w: the splats' radius over the particles' (above 1: a sheet one particle thick drawn whole)
};

Texture2D<float> g_points : register(t3);        // the shared texture
Buffer<float4> g_normals : register(t4);         // the mesh's normals (CsNormals)
Texture2D<float> g_meshDepth : register(t5);     // the cloth's and the jelly's linear depth (1e9: none)
Texture2D<float> g_fluidDepth : register(t6);    // the water's front, linear depth (1e9: none)
Texture2D<float> g_fluidThick : register(t7);    // the water's thickness (metres, about)

float3 Point(uint i) {
    uint p = g_rows.x + i;
    uint2 at = uint2((p % 1024) * 4, p / 1024);
    return float3(g_points.Load(int3(at, 0)), g_points.Load(int3(at + uint2(1, 0), 0)), g_points.Load(int3(at + uint2(2, 0), 0))) + g_origin.xyz;
}

// What hides a point at linear depth z on a pixel: the world, the meshes.
float Occluder(float2 pixel) { return min(SceneDepth(pixel), g_meshDepth.Load(int3(pixel, 0))); }

float4 Project(float3 world) {
    float3 d = world - g_eye.xyz;
    float z = dot(d, g_fwd.xyz);
    float2 ndc = float2(dot(d, g_right.xyz) * g_proj.x / z + g_proj.z, dot(d, g_up.xyz) * g_proj.y / z + g_proj.w);
    // (clip space with w = z: a depth of z / 1000 for the mod's own depth buffer, kept within 0..1)
    return float4(ndc * z, saturate(z / 1000) * z, z);
}

// ---- the meshes' normals: each vertex the sum of its triangles' (adjacency: CSR, the triangles of vertex v at
// g_adjacency[g_adjStart[v] .. g_adjStart[v + 1]))
Buffer<uint> g_index : register(t8);
Buffer<uint> g_adjStart : register(t9);
Buffer<uint> g_adjacency : register(t10);
RWBuffer<float4> g_outNormals : register(u0);
[numthreads(64, 1, 1)] void CsNormals(uint3 id : SV_DispatchThreadID) {
    uint v = id.x;
    if (v >= g_rows.y) return;
    float3 n = 0;
    for (uint k = g_adjStart[v]; k < g_adjStart[v + 1]; k++) {
        uint t = g_adjacency[k] * 3;
        float3 a = Point(g_index[t]), b = Point(g_index[t + 1]), c = Point(g_index[t + 2]);
        n += cross(b - a, c - a);   // (area-weighted)
    }
    g_outNormals[v] = float4(normalize(n + float3(0, 1e-12, 0)), 0);
}

// ---- the meshes
struct MeshV { float4 pos : SV_Position; float3 world : WORLD; float3 normal : NORMAL; float z : DEPTH; };
MeshV VsMesh(uint v : SV_VertexID) {
    MeshV o;
    o.world = Point(v);
    o.normal = g_normals[v].xyz;
    o.pos = Project(o.world);
    o.z = o.pos.w;
    return o;
}
struct MeshOut { float4 color : SV_Target0; float z : SV_Target1; };
MeshOut PsMesh(MeshV i) {
    if (i.z > SceneDepth(i.pos.xy) + 0.02) discard;
    float3 n = normalize(i.normal);
    float3 toEye = normalize(g_eye.xyz - i.world);
    if (dot(n, toEye) < 0) n = -n;   // (two-sided: a cloth's back)
    float3 base = g_rows.w == 1 ? float3(0.85, 0.12, 0.10) : float3(0.78, 0.74, 0.62);   // (the jelly's red, the cloth's canvas)
    float3 sun = Lin(g_sunColor.rgb) * saturate(dot(n, g_sun.xyz));
    float3 amb = Lin(g_ambient.rgb);
    float3 h = normalize(g_sun.xyz + toEye);
    float spec = pow(saturate(dot(n, h)), g_rows.w == 1 ? 60 : 8) * (g_rows.w == 1 ? 0.6 : 0.05);
    float3 c = Lin(base) * (sun + amb) + Lin(g_sunColor.rgb) * spec;
    MeshOut o;
    o.color = float4(ApplyFog(Enc(c), i.z), 1);
    o.z = i.z;
    return o;
}

// ---- the water's particles: quads facing the camera, a sphere's front in each. Drawn larger than the particles
// (g_water.w): water spread thin on the ground is one particle thick, its particles a spacing apart, and drawn at
// their own size they leave the ground showing between them (specks, not a sheet).
float SplatRadius() { return g_origin.w * max(g_water.w, 1); }
struct SplatV { float4 pos : SV_Position; float2 uv : UV; float z : DEPTH; };
SplatV VsSplat(uint v : SV_VertexID) {
    uint i = v / 6, c = v % 6;
    float2 corner = float2(c == 1 || c == 4 || c == 5 ? 1 : -1, c == 2 || c == 3 || c == 5 ? 1 : -1);
    float r = SplatRadius();
    float3 world = Point(i) + (g_right.xyz * corner.x + g_up.xyz * corner.y) * r;
    SplatV o;
    o.pos = Project(world);
    o.uv = corner;
    o.z = dot(Point(i) - g_eye.xyz, g_fwd.xyz);
    return o;
}
struct DepthOut { float z : SV_Target0; float depth : SV_Depth; };
DepthOut PsSplatDepth(SplatV i) {
    float d2 = dot(i.uv, i.uv);
    if (d2 > 1) discard;
    float z = i.z - sqrt(1 - d2) * SplatRadius();
    if (z > Occluder(i.pos.xy)) discard;
    DepthOut o;
    o.z = z;
    o.depth = saturate(z / 1000);
    return o;
}
float PsSplatThick(SplatV i) : SV_Target {
    float d2 = dot(i.uv, i.uv);
    if (d2 > 1) discard;
    if (i.z - sqrt(1 - d2) * SplatRadius() > Occluder(i.pos.xy)) discard;
    float k = 1 / max(g_water.w, 1);
    return 2 * sqrt(1 - d2) * SplatRadius() * k * k * k;   // (the water's own volume: a larger splat, spread thinner)
}

// ---- the depth smoothed: a bilateral Gaussian along one direction, its width the particles' size on the screen
float PsSmooth(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    float z = g_fluidDepth.Load(int3(p, 0));
    if (z > 1e8) return z;
    float radius = clamp(g_water.x / z, 1, 24);
    float2 dir = g_rows.z ? float2(0, 1) : float2(1, 0);
    float sum = 0, wsum = 0, sigma = radius * 0.5, falloff = g_water.y;
    for (int k = -24; k <= 24; k++) {
        if (abs(k) > radius) continue;
        float s = g_fluidDepth.Load(int3(p + dir * k, 0));
        if (s > 1e8) continue;
        float w = exp(-k * k / (2 * sigma * sigma)) * exp(-(s - z) * (s - z) * falloff);
        sum += s * w;
        wsum += w;
    }
    return wsum > 0 ? sum / wsum : z;
}

// ---- the water's surface over the frame
float3 FluidPoint(float2 p) {
    float z = g_fluidDepth.Load(int3(p, 0));
    return z > 1e8 ? float3(0, 0, 0) : g_eye.xyz + ViewRay(p) * z;
}
float4 PsFluid(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    float z = g_fluidDepth.Load(int3(p, 0));
    if (z > 1e8) discard;
    float3 P = g_eye.xyz + ViewRay(p) * z;
    // the normal: the smaller of the two differences on each axis (no edge between two bodies of water)
    float zl = g_fluidDepth.Load(int3(p - float2(1, 0), 0)), zr = g_fluidDepth.Load(int3(p + float2(1, 0), 0));
    float zu = g_fluidDepth.Load(int3(p - float2(0, 1), 0)), zd = g_fluidDepth.Load(int3(p + float2(0, 1), 0));
    float3 dx = abs(zr - z) < abs(z - zl) && zr < 1e8 ? FluidPoint(p + float2(1, 0)) - P : P - FluidPoint(p - float2(1, 0));
    float3 dy = abs(zd - z) < abs(z - zu) && zd < 1e8 ? FluidPoint(p + float2(0, 1)) - P : P - FluidPoint(p - float2(0, 1));
    if (zl > 1e8 && zr > 1e8) dx = g_right.xyz;
    if (zu > 1e8 && zd > 1e8) dy = -g_up.xyz;
    float3 n = normalize(cross(dy, dx));
    float3 toEye = normalize(g_eye.xyz - P);
    if (dot(n, toEye) < 0) n = -n;
    float thick = g_fluidThick.Load(int3(p, 0)) * g_water.z;
    // Water spread thin lies flat on what is under it: its surface takes that surface's normal (the world's own, from
    // its depth), the particles' bumps only where it has depth. A film on the asphalt mirrors the sky evenly.
    float3 g0 = ScenePoint(p), gx = ScenePoint(p + float2(1, 0)) - g0, gy = ScenePoint(p + float2(0, 1)) - g0;
    float3 ground = normalize(cross(gy, gx));
    if (dot(ground, toEye) < 0) ground = -ground;
    const float film = SplatRadius() * 2;   // (a sheet one splat thick and less: a film)
    n = normalize(lerp(ground, n, saturate(thick / film - 0.5)));
    // the frame behind it, bent by the surface and darkened by the water's depth (Beer's law); a wet surface is
    // darker than a dry one (less light scattered back from its pores): the frame under a film too
    float2 bend = float2(dot(n, g_right.xyz), -dot(n, g_up.xyz));   // (the normal on the screen)
    float2 uv = (p + bend * min(thick, 1) * 40) * g_size.zw;
    float3 behind = Lin(g_sceneColor.SampleLevel(g_linear, saturate(uv), 0).rgb) * lerp(1, 0.55, saturate(thick / 0.01));
    float3 absorb = exp(-thick * float3(1.6, 0.5, 0.35));
    float3 refracted = behind * absorb + Lin(float3(0.10, 0.22, 0.25)) * (1 - absorb) * Lin(g_ambient.rgb + g_sunColor.rgb * 0.3);
    // the sky reflected (the fog's colour as the sky's, as the game's own distance has it) and the sun's glint
    float fres = 0.02 + 0.98 * pow(1 - saturate(dot(n, toEye)), 5);
    float3 sky = Lin(g_fogColor.w > 0.5 ? g_fogColor.rgb : float3(0.55, 0.62, 0.70));
    float3 h = normalize(g_sun.xyz + toEye);
    float3 glint = Lin(g_sunColor.rgb) * pow(saturate(dot(n, h)), 300) * 3;
    float3 c = lerp(refracted, sky, fres) + glint;
    // its edge: where the sheet thins out to nothing it fades (premultiplied over the frame) instead of ending sharp
    float cover = saturate(thick / (film * 0.15));
    return float4(ApplyFog(Enc(c), z), 1) * cover;
}
