// river_water's water (river_water.cpp): the game's river drawn anew on its own triangles, captured from LS3D.
#include "mml/gfx/frame.hlsli"

Texture2D<float4> g_refl : register(t2);   // the reflection: the scene from the camera mirrored in the water's plane

cbuffer Water : register(b1) {
    row_major float4x4 g_vp;    // the frame's view x projection (row vectors)
    row_major float4x4 g_rvp;   // the reflection's (MmlGfxView::view x proj)
    float4 g_water;             // x the water's level, y the waves' strength, z 1 a reflection this frame, w the time
    float4 g_deep;              // rgb the colour of deep water, a: metres of water to reach it
    float4 g_shallow;           // rgb what the riverbed is tinted by, a: the bends' strength (pixels)
};

struct VOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
};

VOut VsWater(float3 p : POSITION) {
    VOut o;
    o.pos = mul(float4(p, 1), g_vp);
    o.world = p;
    return o;
}

// Directional waves (their slopes summed): the surface's normal at a point. A wave shorter than a few of the pixel's
// metres there fades out (`pixel`: metres a pixel covers): drawn, it would alias into stripes.
float3 WaveNormal(float2 xz, float t, float pixel) {
    const float4 waves[8] = {   // xy direction, z wavelength (m), w speed (m/s)
        float4(0.83, 0.55, 9.0, 1.9), float4(-0.42, 0.91, 5.3, 1.4), float4(0.15, -0.99, 3.1, 1.1),
        float4(-0.95, -0.31, 1.9, 0.85), float4(0.6, -0.8, 1.15, 0.65), float4(0.99, 0.12, 0.7, 0.5),
        float4(-0.7, 0.71, 0.43, 0.4), float4(0.31, 0.95, 0.27, 0.3)};
    float2 slope = 0;
    [unroll] for (int i = 0; i < 8; i++) {
        float k = 6.2831853 / waves[i].z;
        float a = 0.016 * waves[i].z * saturate(waves[i].z / (pixel * 6) - 1);   // steepness alike at every scale
        float ph = k * (dot(waves[i].xy, xz) - waves[i].w * t);
        slope += waves[i].xy * (a * k * cos(ph));
    }
    slope *= g_water.y;
    return normalize(float3(-slope.x, 1, -slope.y));
}

float4 PsWater(VOut i) : SV_Target {
    float2 px = i.pos.xy;
    float3 toEye = g_eye.xyz - i.world;
    float z = dot(-toEye, g_fwd.xyz);                // this point's linear view depth
    float behind = SceneDepth(px);                   // what the frame drew here (the game's water hidden: the bed)
    if (z > behind + 0.02) discard;                  // something stands in front of the water
    float dist = length(toEye);
    float3 v = toEye / dist;
    float pixel = dist * 2 / (g_proj.y * g_size.y) / max(abs(dot(v, float3(0, 1, 0))), 0.15);   // (along the surface)
    float3 n = WaveNormal(i.world.xz, g_water.w, pixel);
    float fresnel = 0.02 + 0.98 * pow(1 - saturate(dot(n, v)), 5);
    // the reflection: this point in the mirrored camera's picture, bent by the waves
    float3 sky = g_fogColor.w > 0.5 ? g_fogColor.rgb : float3(0.55, 0.62, 0.7);
    float3 refl = sky;
    if (g_water.z > 0.5) {
        float4 c = mul(float4(i.world, 1), g_rvp);
        float2 uv = c.xy / c.w * float2(0.5, -0.5) + 0.5 + n.xz * 0.04;
        refl = g_refl.SampleLevel(g_linear, saturate(uv), 0).rgb;
    }
    // the water's body: the bed seen through it, bent, darker and greener the more water lies over it
    float2 bent = px + n.xz * g_shallow.a;
    float bedZ = SceneDepth(bent);
    if (bedZ < z) bent = px, bedZ = behind;          // (bent onto something in front: not bent)
    float3 bed = g_sceneColor.SampleLevel(g_linear, bent * g_size.zw, 0).rgb;
    float thick = saturate((bedZ - z) * abs(dot(v, float3(0, 1, 0))) / max(g_deep.a, 0.1));
    float3 body = lerp(Lin(bed) * Lin(g_shallow.rgb), Lin(g_deep.rgb), thick);
    // the sun's glint
    float3 h = normalize(g_sun.xyz + v);
    float glint = pow(saturate(dot(n, h)), 400) * 3 * g_sunColor.rgb.g;
    float3 col = lerp(body, Lin(refl), fresnel) + glint;
    return float4(ApplyFog(Enc(col), z), 1);
}
