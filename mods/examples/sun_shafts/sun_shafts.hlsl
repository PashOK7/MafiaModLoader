// sun_shafts' passes (sun_shafts.cpp): the sun's light scattered by the air, where the sun reaches it (shafts between
// the buildings, the shadows of cars and trees standing in the haze), from the frame's depth and the sun's shadow map.
//
//   PsMarch      half the frame's size, two targets: the light scattered towards the eye along each pixel's ray
//                (rgb) and how much of what lies behind is still seen (a: the transmittance), and the depth that ray
//                ended at (the 2x2 pixels' nearest or farthest, by a checkerboard: the edges of both kept)
//   PsBlurX/Y    a blur across the half-size picture that does not cross depth edges (the march's dither removed)
//   PsComposite  the frame's size: each pixel takes the half-size samples of its own depth (a joint bilateral
//                upsample), premultiplied over the frame: frame * transmittance + light
#include "mml/gfx/frame.hlsli"

cbuffer Shafts : register(b1) {
    float4 g_light;     // rgb: the light's colour (the sun's times the tint times the intensity), a: the air's density (1/m)
    float4 g_march;     // x: steps, y: the farthest a ray goes (m), z: the anisotropy (Henyey-Greenstein g), w: how much the haze dims what is behind (0..1)
    float4 g_half;      // the half-size picture: width, height, 1/width, 1/height
    float4 g_debug;     // x 1: the march's light is the part of the ray the sun reaches (grey), for looking at the shadows in the air
};

Texture2D<float4> g_scatter : register(t3);   // PsMarch's or a blur's light (rgb) and transmittance (a)
Texture2D<float> g_halfDepth : register(t4);  // PsMarch's depth (linear view depth, m)

// How much of the sun's light the air scatters towards the eye for an angle between the view and the sun, per
// steradian (Henyey-Greenstein, normalised: over the sphere it adds up to 1): forward (towards the sun) much more.
float Phase(float cosTheta, float g) {
    float g2 = g * g;
    return (1 - g2) / (4 * 3.14159265 * pow(max(1 + g2 - 2 * g * cosTheta, 1e-4), 1.5));
}

// A dither per pixel that changes little from a pixel to the next (interleaved gradient noise): the steps start at
// another place in each pixel, so the march's bands become a fine noise the blur takes away.
float Dither(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

struct MarchOut {
    float4 light : SV_Target0;
    float depth : SV_Target1;
};

MarchOut PsMarch(float4 pos : SV_Position) {
    MarchOut o;
    // the full-size pixels under this one: their nearest or farthest depth, alternating as a checkerboard
    int2 full = int2(pos.xy) * 2;
    float z0 = SceneDepth(full), z1 = SceneDepth(full + int2(1, 0)), z2 = SceneDepth(full + int2(0, 1)), z3 = SceneDepth(full + int2(1, 1));
    bool farthest = ((int(pos.x) + int(pos.y)) & 1) != 0;
    float z = farthest ? max(max(z0, z1), max(z2, z3)) : min(min(z0, z1), min(z2, z3));
    o.depth = z;
    o.light = float4(0, 0, 0, 1);
    if (g_debug.x > 1.5) {   // the map itself across the screen, and where this pixel's point falls on it
        // at the point this pixel shows: red, the map holds nothing there (1); green, how far (metres / 20) the map's
        // nearest surface is from the point along the sun's ray; blue, the point is off the map
        float3 c = SunShadowCoord(ScenePoint(float2(full) + 1));
        bool off = any(c.xy <= 0) || any(c.xy >= 1);
        float stored = g_sunShadow.Load(int3(saturate(c.xy) * g_sunShadowInfo.w, 0));
        o.light = float4(stored >= 0.9999 ? 1 : 0, saturate(abs(stored - c.z) * 800 / 20), off ? 1 : 0, 0);
        return o;
    }
    if (g_sunShadowInfo.x < 0.5 || g_sun.y <= 0) return o;   // no map this frame (no sun above the horizon)
    float2 pixel = float2(full) + 1;                          // the 2x2's centre
    float3 ray = ViewRay(pixel);                              // forward component 1: a point at depth z is eye + ray * z
    float rayLen = length(ray);
    float3 dir = ray / rayLen;
    float dist = min(IsSky(z) ? g_march.y : z * rayLen, g_march.y);
    int steps = int(g_march.x);
    float dt = dist / steps;
    float sigma = g_light.a;
    float stepT = exp(-sigma * dt);                           // the transmittance of one step
    float t = dt * Dither(pos.xy);
    float T = 1, inscatter = 0, litSteps = 0;
    [loop] for (int i = 0; i < steps; i++) {
        float3 p = g_eye.xyz + dir * t;
        // the light this step scatters towards the eye, dimmed by the air between it and the eye (the integral of
        // sigma * T over the step, exactly)
        float lit = SunLit(p);
        inscatter += lit * T * (1 - stepT);
        litSteps += lit;
        T *= stepT;
        t += dt;
    }
    float3 light = g_light.rgb * Phase(dot(dir, g_sun.xyz), g_march.z) * inscatter;
    o.light = float4(light, lerp(1, T, g_march.w));
    if (g_debug.x > 0.5) o.light = float4((litSteps / steps).xxx, 0);   // (composited as an opaque grey)
    return o;
}

// A 9-tap blur along one axis, each sample weighed by how close its depth is to this pixel's (relative: the same
// tolerance near and far), so the haze in front of a wall does not bleed onto the sky beside it.
float4 Blur(float2 pos, int2 axis) {
    int2 at = int2(pos);
    float z = g_halfDepth.Load(int3(at, 0));
    static const float kW[5] = {0.2270270, 0.1945946, 0.1216216, 0.0540540, 0.0162162};
    float4 sum = g_scatter.Load(int3(at, 0)) * kW[0];
    float wsum = kW[0];
    int2 last = int2(g_half.xy) - 1;
    [unroll] for (int i = 1; i < 5; i++)
        [unroll] for (int s = -1; s <= 1; s += 2) {
            int2 q = clamp(at + axis * i * s, 0, last);
            float zq = g_halfDepth.Load(int3(q, 0));
            float w = kW[i] * exp(-abs(zq - z) / (0.05 * max(z, 1)));
            sum += g_scatter.Load(int3(q, 0)) * w;
            wsum += w;
        }
    return sum / wsum;
}
float4 PsBlurX(float4 pos : SV_Position) : SV_Target { return Blur(pos.xy, int2(1, 0)); }
float4 PsBlurY(float4 pos : SV_Position) : SV_Target { return Blur(pos.xy, int2(0, 1)); }

float4 PsComposite(float4 pos : SV_Position) : SV_Target {
    float z = SceneDepth(pos.xy);
    // the four half-size samples around this pixel, weighed by distance (bilinear) and by depth
    float2 h = pos.xy * 0.5 - 0.5;
    int2 h0 = int2(floor(h));
    float2 f = h - h0;
    int2 last = int2(g_half.xy) - 1;
    float4 sum = 0;
    float wsum = 0;
    float best = 1e30;
    float4 nearest = 0;
    [unroll] for (int y = 0; y < 2; y++)
        [unroll] for (int x = 0; x < 2; x++) {
            int2 q = clamp(h0 + int2(x, y), 0, last);
            float zq = g_halfDepth.Load(int3(q, 0));
            float4 s = g_scatter.Load(int3(q, 0));
            float dz = abs(zq - z);
            float w = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y) / (dz / max(z, 1) + 1e-3);
            sum += s * w;
            wsum += w;
            if (dz < best) { best = dz; nearest = s; }
        }
    // no sample of this depth near (a thin edge): the closest in depth, not a blend across the edge
    float4 s = best > 0.1 * max(z, 1) ? nearest : sum / max(wsum, 1e-6);
    if (g_debug.x > 0.5) return float4(s.rgb, 1 - s.a);
    // the frame is not HDR: the light's own soft curve (a little light as it is, much of it towards white, never past
    // it) instead of the hardware's clip
    float3 light = 1 - exp(-s.rgb);
    return float4(light, 1 - s.a);   // premultiplied: frame * T + light
}
