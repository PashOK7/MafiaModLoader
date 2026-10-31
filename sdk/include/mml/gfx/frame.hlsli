// What every Direct3D 11 pass of the graphics bridge gets (mml.h gfx_pass, MmlGfxFrame; docs/MODDING.md "Effects
// over the frame"), shader model 5. A mod's shader: #include "mml/gfx/frame.hlsli" (mml_add_mod's SHADERS have the
// SDK on their include path).
//
// World space is the game's (metres, y up). Linear view depth is the distance along the camera's forward axis. A pass
// draws into the effects target: premultiplied RGBA (rgb already times alpha; alpha: how much of the frame it covers),
// composited over the frame as frame * (1 - alpha) + rgb, where the world is drawn and the HUD is not yet. The passes
// are laid over each other in their order the same way (the bridge's blend state: ONE, INV_SRC_ALPHA), so a pass that
// returns 0 leaves what the passes before it drew.

cbuffer MmlFrame : register(b0) {
    float4 g_eye;         // the camera's position, w: the time in seconds
    float4 g_right;       // the camera's axes in the world (unit), w unused
    float4 g_up;
    float4 g_fwd;
    float4 g_proj;        // the projection's _11, _22, _31, _32 (row vectors)
    float4 g_size;        // width, height, 1/width, 1/height (pixels)
    float4 g_sun;         // the direction towards the sun (unit), w: 1 the game's (its Direct3D light, else the scene's sun), 0 a default
    float4 g_depthProj;   // the projection's _33, _43; the linear depth of the sky (where nothing was drawn); unused
    float4 g_sunColor;    // the sun's colour, w: 1 the game's
    float4 g_fog;         // the world's fog: start, end (linear), density (exp), mode (0 none, 1 exp, 2 exp2, 3 linear)
    float4 g_fogColor;    // its colour, w: 1 when on
    float4 g_ambient;     // the ambient light, w: 1 the scene's (else a default grey)
    // the shadow map from the sun (mml.h shader_shadows: while a mod wants it), g_sunShadow
    row_major float4x4 g_sunShadowViewProj;   // the world to the map's clip space (x, y -1..1, z 0..1 its depth)
    float4 g_sunShadowInfo;   // x 1 when the map was drawn for this frame, y one texel (1 / size), z the depth's bias, w size
    // towards the sun the sky shows (its lens flare), unit; w: 1 found, 0 none (then g_sun's). The game lights the world
    // and baked its shadows from g_sun, elsewhere in the sky: what is seen in the sun (a reflection of it, glare, light
    // through water or leaves) goes towards g_sunSeen, what is lit and shadowed as the world is towards g_sun.
    float4 g_sunSeen;
    float4 g_atmosphere;       // clear distance, extinction / m, base height, inverse scale height
    float4 g_atmosphereColor;  // display-space tint, directional scattering
    float4 g_atmosphereInfo;   // enabled, sky integration distance, maximum opacity, sky horizon blend
    float4 g_atmospherePhase;  // anisotropy, painted-sky angular falloff, reserved, reserved
};

Texture2D<float> g_sceneDepth : register(t0);    // linear view depth, metres (the sky: g_depthProj.z)
Texture2D<float4> g_sceneColor : register(t1);   // the frame as drawn so far (the world, no HUD)
Texture2D<float> g_sunShadow : register(t2);     // the sun's depth (1 nothing); unbound when g_sunShadowInfo.x is 0
// Each pixel's motion in pixels: this pixel plus it is where the surface it shows was the frame before (the camera's
// and what moved by itself); bound while a mod asked for it (mml.h gfx_motion) and the frame before was drawn, else
// unbound (it reads 0: no motion). Not in a scene view's passes.
Texture2D<float2> g_motion : register(t3);
SamplerState g_point : register(s0);
SamplerState g_linear : register(s1);

// Where the surface at this pixel was on the screen the frame before (pixels; the motion vectors).
float2 PixelBefore(float2 pixel) { return pixel + g_motion.Load(int3(pixel, 0)); }

// A full-screen triangle: Draw(3), no buffers (the bridge binds it as the vertex shader before each pass).
float4 VsFullScreen(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// The world direction through a pixel, scaled so that its forward component is 1: the point at linear view depth z
// is g_eye.xyz + ViewRay(pixel) * z.
float3 ViewRay(float2 pixel) {
    float2 ndc = float2(pixel.x * g_size.z * 2 - 1, 1 - pixel.y * g_size.w * 2);
    float vx = (ndc.x - g_proj.z) / g_proj.x;
    float vy = (ndc.y - g_proj.w) / g_proj.y;
    return g_right.xyz * vx + g_up.xyz * vy + g_fwd.xyz;
}

float SceneDepth(float2 pixel) { return g_sceneDepth.Load(int3(pixel, 0)); }
bool IsSky(float z) { return z >= g_depthProj.z * 0.5; }

// The world point a pixel shows (where the scene was drawn).
float3 ScenePoint(float2 pixel) { return g_eye.xyz + ViewRay(pixel) * SceneDepth(pixel); }

// How much of the world's fog lies in front of a point at linear view depth z (0 none, 1 all), as Direct3D's
// fixed-function fog has it: effects fade into the distance as the world does.
float FogAmount(float z) {
    if (g_fogColor.w < 0.5) return 0;
    if (g_fog.w > 2.5) return saturate((z - g_fog.x) / max(g_fog.y - g_fog.x, 1e-3));
    float d = g_fog.z * z;
    return g_fog.w > 1.5 ? 1 - exp(-d * d) : 1 - exp(-d);
}
float3 ApplyFog(float3 color, float z) { return lerp(color, g_fogColor.rgb, FogAmount(z)); }

// The game draws in display space (gamma about 2.2): add and mix light in linear terms (Lin), back for the frame (Enc).
float3 Lin(float3 c) { return pow(max(c, 0), 2.2); }
float3 Enc(float3 c) { return pow(max(c, 0), 1 / 2.2); }

#include "atmosphere.hlsli"

// A world point on the screen (pixels); z: its linear view depth.
float3 ToScreen(float3 p) {
    float3 d = p - g_eye.xyz;
    float z = dot(d, g_fwd.xyz);
    float2 ndc = float2(dot(d, g_right.xyz) * g_proj.x / z + g_proj.z, dot(d, g_up.xyz) * g_proj.y / z + g_proj.w);
    return float3((ndc.x + 1) * 0.5 * g_size.x, (1 - ndc.y) * 0.5 * g_size.y, z);
}

// The sun's shadow map at a world point: xy the map's uv, z the depth to compare with (the bias taken off).
float3 SunShadowCoord(float3 world) {
    float4 p = mul(float4(world, 1), g_sunShadowViewProj);
    return float3(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5, p.z - g_sunShadowInfo.z);
}

// Whether the sun reaches a world point, one texel: 1 lit, 0 in a shadow (1 off the map, or with no map). For many
// samples a pixel: a ray marched through the air.
float SunLit(float3 world) {
    if (g_sunShadowInfo.x < 0.5) return 1;
    float3 c = SunShadowCoord(world);
    if (any(c.xy <= 0) || any(c.xy >= 1)) return 1;
    return g_sunShadow.Load(int3(c.xy * g_sunShadowInfo.w, 0)) >= c.z ? 1 : 0;
}

// The same for a surface, filtered: a box of 3x3 texels around the point, the texels it covers in part weighted by
// how much (16 compared), so the shadow's edge is soft and does not crawl as the point moves.
float SunShadow(float3 world) {
    if (g_sunShadowInfo.x < 0.5) return 1;
    float3 c = SunShadowCoord(world);
    if (any(c.xy <= 0) || any(c.xy >= 1)) return 1;
    float size = g_sunShadowInfo.w;
    float2 t = c.xy * size - 0.5;   // texel space (texel centres at whole numbers)
    int2 i0 = int2(floor(t));
    float2 f = t - i0;
    float wx[4] = {1 - f.x, 1, 1, f.x};
    float wy[4] = {1 - f.y, 1, 1, f.y};
    int last = int(size) - 1;
    float lit = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++) {
            int2 at = clamp(i0 + int2(x - 1, y - 1), 0, last);
            lit += (g_sunShadow.Load(int3(at, 0)) >= c.z ? 1 : 0) * wx[x] * wy[y];
        }
    return lit / 9;
}

// Directional-light PCSS: find blockers, then widen the filter by their distance from the receiving surface.
// angularRadius is tan of the light's half-angle (the sun: 0.00465). Matrix scales supply metres without new ABI
// constants; the sampling pattern is fixed, with no frame-random noise. Existing SunShadow stays a fixed PCF filter.
float SunShadowSoft(float3 world, float angularRadius) {
    if (g_sunShadowInfo.x < 0.5) return 1;
    if (angularRadius <= 0) return SunShadow(world);
    float3 c = SunShadowCoord(world);
    if (any(c.xy <= 0) || any(c.xy >= 1)) return 1;
    float depthScale = length(float3(g_sunShadowViewProj[0][2], g_sunShadowViewProj[1][2], g_sunShadowViewProj[2][2]));
    float2 uvScale = 0.5 * float2(length(float3(g_sunShadowViewProj[0][0], g_sunShadowViewProj[1][0], g_sunShadowViewProj[2][0])),
                                length(float3(g_sunShadowViewProj[0][1], g_sunShadowViewProj[1][1], g_sunShadowViewProj[2][1])));
    if (depthScale < 1e-7 || any(uvScale < 1e-7)) return SunShadow(world);
    float texel = g_sunShadowInfo.y;
    float2 search = clamp(angularRadius * min(c.z / depthScale, 200) * uvScale, 2 * texel, 32 * texel);
    float blockers = 0, sum = 0;
    [unroll] for (int b = 0; b < 17; ++b) {
        float r = b == 0 ? 0 : sqrt(max(0, (b - 0.5) / 16.0));
        float angle = b * 2.39996323;
        float2 uv = c.xy + float2(cos(angle), sin(angle)) * (r * search);
        if (all(uv > 0) && all(uv < 1)) {
            float z = g_sunShadow.Load(int3(uv * g_sunShadowInfo.w, 0));
            if (z < c.z) { sum += z; blockers += 1; }
        }
    }
    if (blockers == 0) return SunShadow(world);   // preserve thin alpha-tested details between search taps
    float gapMetres = max(0, c.z - sum / blockers) / depthScale;
    float2 radius = clamp(gapMetres * angularRadius * uvScale, texel, 32 * texel);
    if (all(radius <= 1.5 * texel)) return SunShadow(world);
    float lit = 0;
    [unroll] for (int s = 0; s < 32; ++s) {
        float r = sqrt((s + 0.5) / 32.0);
        float angle = s * 2.39996323;
        float2 uv = c.xy + float2(cos(angle), sin(angle)) * (r * radius);
        lit += any(uv <= 0) || any(uv >= 1) ? 1 :
               (g_sunShadow.Load(int3(uv * g_sunShadowInfo.w, 0)) >= c.z ? 1 : 0);
    }
    return lit / 32;
}
