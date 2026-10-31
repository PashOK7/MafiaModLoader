// river_water's water (river_water.cpp): the game's water drawn anew on its own triangles, captured from LS3D, by the
// SDK's water pieces (mml/gfx/water.hlsli).
//
// Per pixel: the wind's ripples (river_waves.hlsl's slopes and shape filtered over the pixel, the crests sharpened by
// the choppy displacement); the planar
// reflection followed along the reflected ray and streaked by the roughness; the opaque water's body (a uniform
// virtual depth, no scene colour transmitted, the light it scatters back, lit as the game lights its own, in the shadows the world
// casts); the sun's light scattered once in the water towards the eye (the glow against the sun, through the
// crests, and the sky and the sun seen through steep crests); whitecaps where crests break; the sun's glitter where
// the sky shows the sun (its lens flare, not the light the world is lit from), out of
// whatever the reflected ray meets; foam where the water meets a wall or a post; the world's fog (the reflection holds
// its own).
#include "mml/gfx/frame.hlsli"
#include "mml/gfx/water.hlsli"

Texture2D<float4> g_refl : register(t4);         // the reflection, its mips (sRGB: read in linear terms)
Texture2D<float> g_reflDepth : register(t5);     // its linear depth
Texture2DArray<float4> g_slopes : register(t6);  // each cascade: slope x, slope z, their squares (mipmapped)
Texture2DArray<float4> g_shape : register(t7);   // each cascade: the height, dDx/dx, dDz/dz, dDx/dz (mipmapped)
Texture2DArray<float4> g_displacement : register(t8); // height, horizontal Dx, Dz
Texture2DArray<float4> g_previousDisplacement : register(t9);
Texture2D<float4> g_skyReflection : register(t10); // linear HDR painted sky, boosted before roughness mips
RWTexture2D<float4> g_skyOutput : register(u0);
SamplerState g_wrap : register(s2);              // anisotropic, wrapping (the waves)
SamplerState g_clamp : register(s3);             // anisotropic, clamped (the reflection)

cbuffer Water : register(b1) {
    row_major float4x4 g_vp;    // the frame's view x projection (row vectors)
    row_major float4x4 g_rvp;   // the reflection's
    float4 g_rdepth;            // a world point's linear depth in the reflection: dot(float4(p, 1), g_rdepth)
    float4 g_water;             // x the reflection's level, y the waves' strength, z 1 a reflection this frame, w time
    float4 g_cascade;           // xyz 1 / each cascade's size (m), w the slopes' variance no cascade draws (an axis)
    float4 g_absorb;            // rgb absorption a metre (linear light), a the uniform virtual depth (m)
    float4 g_scatter;           // rgb the light the water's body sends back of white light (linear), a the foam
    float4 g_light;             // x the sun's glitter, y 1 the reflection's copy reads display-encoded, z the debug
                                // view (0 none, 1 the reflection, 2 the normal, 3 the body, 4 Fresnel, 5 roughness, 6 the
                                // reflection unblurred, 7 the sun scattered in the water and through the crests, 8 the
                                // shadows, 9 the Jacobian, 10 the reflection where flat water would show it, 11 how far the reflected ray went, 12 the
                                // reflection's depth and the point's in it (m/100), 13 the slopes), w the waves' chop (their displacement towards the crests)
    float4 g_reflSize;          // the reflection's width, height, 1 / width, 1 / height
    float4 g_optics;            // x the particles' scattering a metre, y its phase's asymmetry g, z the crests' glow,
                                // w the crests' height (2 deviations of the heights, m)
    row_major float4x4 g_rinv;  // the reflection's view x projection inverted
    float4 g_shadow;           // x: shadow penumbra scale, y: cosine of the painted solar glow's outer radius
    float4 g_geometry;         // x height multiplier, y 3D enabled, z conservative wave bound, w unused
    row_major float4x4 g_previousVP; // unjittered camera of the preceding water draw
    float4 g_previousEye;
    float4 g_temporal;         // xy current projection jitter (render pixels), z valid history, w temporal MRT bound
};

// Expand the actual painted sky's highlights before filtering. The sun frame is only an anchor: placing an
// angular disc there invents a clear sun even when its painting is a bright opening behind a cloud. A bounded
// inverse-tone curve keeps the painting's shape and hue, including cloud coverage. This is an LDR-to-HDR
// reconstruction assumption, not recovered physical radiance; an unclipped HDR sky would not need it.
[numthreads(8, 8, 1)]
void CsPaintedSky(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)g_reflSize.xy)) return;
    float3 color = g_refl.Load(int3(id.xy, 0)).rgb;
    if (g_light.y > 0.5) color = Lin(color);
    if (IsSky(g_reflDepth.Load(int3(id.xy, 0))) && (g_sunSeen.w > 0.5 || g_sun.w > 0.5)) {
        float2 uv = (float2(id.xy) + 0.5) * g_reflSize.zw;
        float2 ndc = uv * float2(2, -2) + float2(-1, 1);
        float4 a = mul(float4(ndc, 0, 1), g_rinv), b = mul(float4(ndc, 1, 1), g_rinv);
        float3 ray = normalize(b.xyz / b.w - a.xyz / a.w);
        // Broad, smooth support around the actual sun anchor; the painting still supplies every bright shape.
        float cone = saturate((dot(ray, WaterSunSeen()) - g_shadow.y) / max(1 - g_shadow.y, 1e-6));
        float angular = cone * cone * (3 - 2 * cone);
        float white = max(max(color.r, color.g), color.b);
        float bright = saturate((white - 0.53) / 0.47); // roughly display 0.75..1, in linear light
        color *= 1 + g_light.x * angular * bright * bright / max(1 - white, 1.0 / 16);
    }
    g_skyOutput[id.xy] = float4(color, 1);
}

struct VOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 base : TEXCOORD1;    // undisplaced material point: waves and mean-plane reflection share this phase
    float shore : TEXCOORD2;
    float4 previous : TEXCOORD3;
};

struct PatchPoint { float3 p : POSITION; };
PatchPoint VsWater(float3 p : POSITION) { PatchPoint o; o.p = p; return o; }

struct PatchFactors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };

float EdgeTess(float3 a, float3 b) {
    float3 ab = b - a;
    float t = saturate(dot(g_eye.xyz - a, ab) / max(dot(ab, ab), 1e-8));
    float dist = length(a + ab * t - g_eye.xyz);
    float spacing = clamp(dist * 0.006, 0.25, 16);
    return clamp(length(ab) / spacing, 1, 64);
}

PatchFactors WaterFactors(InputPatch<PatchPoint, 3> p) {
    PatchFactors f;
    float4 a = mul(float4(p[0].p, 1), g_vp), b = mul(float4(p[1].p, 1), g_vp), c = mul(float4(p[2].p, 1), g_vp);
    // Conservative clip rejection includes displacement; keep patches crossing the near plane.
    float pad = max(g_geometry.z, 0.1) * max(g_proj.x, g_proj.y) * 2;
    bool outside = (max(a.w, max(b.w, c.w)) < -pad) ||
        (max(a.x + a.w, max(b.x + b.w, c.x + c.w)) < -pad) ||
        (max(a.w - a.x, max(b.w - b.x, c.w - c.x)) < -pad) ||
        (max(a.y + a.w, max(b.y + b.w, c.y + c.w)) < -pad) ||
        (max(a.w - a.y, max(b.w - b.y, c.w - c.y)) < -pad);
    f.edge[0] = outside ? 0 : EdgeTess(p[1].p, p[2].p);
    f.edge[1] = outside ? 0 : EdgeTess(p[2].p, p[0].p);
    f.edge[2] = outside ? 0 : EdgeTess(p[0].p, p[1].p);
    f.inside = max(f.edge[0], max(f.edge[1], f.edge[2]));
    return f;
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("WaterFactors")]
[maxtessfactor(64.0)]
PatchPoint HsWater(InputPatch<PatchPoint, 3> p, uint id : SV_OutputControlPointID) { return p[id]; }

float ShoreWeight(float3 base) {
    if (g_water.z < 0.5 || abs(base.y - g_water.x) > 0.05) return 1;
    float2 uv = WaterReflectionUv(base, g_rvp);
    if (any(uv < 0) || any(uv > 1)) return 1;
    float z = dot(float4(base, 1), g_rdepth);
    if (z <= 0.05) return 1;
    float depth = g_reflDepth.SampleLevel(g_clamp, uv, 0);
    // The reflected wall's distance from the mean surface. Pin its cut foot, open water gets full waves.
    float gap = max(depth - z, 0) * length(base - g_eye.xyz) / max(z, 0.05);
    return smoothstep(0.1, 1.0, gap);
}

[domain("tri")]
VOut DsWater(PatchFactors f, float3 bary : SV_DomainLocation, const OutputPatch<PatchPoint, 3> patch) {
    VOut o;
    float3 base = patch[0].p * bary.x + patch[1].p * bary.y + patch[2].p * bary.z;
    o.base = base;
    o.shore = ShoreWeight(base);
    float3 delta = 0;
    float dist = length(base - g_eye.xyz);
    // Filter wavelengths smaller than tessellated edges; fade geometry smoothly into the far surface.
    float spacing = clamp(dist * 0.006, 0.25, 16);
    [unroll] for (int c = 0; c < 3; c++) {
        float mip = max(log2(spacing * g_cascade[c] * 256), 0);
        delta += g_displacement.SampleLevel(g_wrap, float3(base.xz * g_cascade[c], c), mip).xyz;
    }
    float strength = g_water.y * g_geometry.x * g_geometry.y * o.shore;
    float3 p = base + float3(-delta.y * g_light.w, delta.x, -delta.z * g_light.w) * strength;
    o.pos = mul(float4(p, 1), g_vp);
    // The world and replacement geometry must sample the same subpixel location before temporal upscaling.
    o.pos.xy += g_temporal.xy * float2(2, -2) * g_size.zw * o.pos.w;
    o.world = p;
    o.previous = 0;
    if (g_temporal.z > 0.5 && g_temporal.w > 0.5) {
        float3 before = 0;
        float spacingBefore = clamp(length(base - g_previousEye.xyz) * 0.006, 0.25, 16);
        [unroll] for (int c = 0; c < 3; c++) {
            float mip = max(log2(spacingBefore * g_cascade[c] * 256), 0);
            before += g_previousDisplacement.SampleLevel(g_wrap, float3(base.xz * g_cascade[c], c), mip).xyz;
        }
        float3 previous = base + float3(-before.y * g_light.w, before.x, -before.z * g_light.w) * strength;
        o.previous = mul(float4(previous, 1), g_previousVP);
    }
    return o;
}

struct WaterPixel { float4 color : SV_Target0; float4 temporal : SV_Target1; };
WaterPixel PsWater(VOut i) {
    float2 px = i.pos.xy;
    float3 p = i.world;
    float3 toEye = g_eye.xyz - p;
    float z = dot(-toEye, g_fwd.xyz);                // this point's linear view depth
    float behind = SceneDepth(px);                   // what the frame drew here (the game's water hidden)
    // the reflection's place on flat water, how it changes from pixel to pixel, and the ripples (before any branch:
    // their derivatives)
    float2 uvFlat = WaterReflectionUv(p, g_rvp);
    float2 uvDy = ddy(uvFlat);
    WaterSurface s = WaterRipples(g_slopes, g_shape, g_wrap, i.base.xz, g_cascade.xyz, g_cascade.w,
                                  g_water.y * g_geometry.x * i.shore, g_light.w);
    if (g_eye.y <= i.base.y) discard;                // underwater remains outside this surface renderer
    if (z > behind + 0.02) discard;                  // something stands in front of the water
    float dist = length(toEye);
    float3 v = toEye / dist;
    float nv = saturate(dot(s.normal, v));
    float fresnel = WaterFresnel(nv);
    float shadow = SunShadowSoft(p, g_shadow.x);
    float under = g_absorb.a;                       // the same virtual depth everywhere; the game has no riverbed
    float3 sunSeen = WaterSunSeen();
    float3 scatterB = g_optics.x;                    // the particles' scattering (grey)

    // the reflection
    float3 r = reflect(-v, s.normal);
    r.y = max(r.y, 1e-3);
    float3 refl, unblurred = 0, flatRefl = 0;
    float hitT = 0;
    bool skyward = true;
    if (g_water.z > 0.5 && abs(i.base.y - g_water.x) < 0.05) {
        float2 up = uvDy / max(length(uvDy), 1e-9) * g_reflSize.w;   // one of the reflection's pixels down the screen
        float pxPerRadian = g_reflSize.y * g_proj.y * 0.5;
        WaterHit hit = WaterFollowReflection(p, r, uvFlat, g_rvp, g_rdepth, g_reflDepth, up, g_rinv, pxPerRadian);
        skyward = hit.sky;
        float spread = WaterReflectionSpread(s.var, hit.t, dist);
        // A contact texel was selected by depth. Filtering it with its uncovered neighbour would put the sky
        // back into the wall's cut foot (a moving slit); roughness spread tends to zero at the contact anyway.
        if (g_geometry.w > 0.5 && !hit.contact && shadow >= 1) {
            refl = WaterStreakedReflection(g_skyReflection, g_clamp, hit.uv, up, spread, nv, px, g_rinv, pxPerRadian);
        } else {
            refl = hit.contact ? g_refl.SampleLevel(g_point, hit.uv, 0).rgb :
                   WaterStreakedReflection(g_refl, g_clamp, hit.uv, up, spread, nv, px, g_rinv, pxPerRadian);
            if (g_light.y > 0.5) refl = Lin(refl);
            if (g_geometry.w > 0.5 && !hit.contact && shadow > 0) {
                float3 lit = WaterStreakedReflection(g_skyReflection, g_clamp, hit.uv, up, spread, nv, px, g_rinv, pxPerRadian);
                refl = lerp(refl, lit, shadow);
            }
        }
        unblurred = g_refl.SampleLevel(g_clamp, hit.uv, 0).rgb;
        flatRefl = g_refl.SampleLevel(g_clamp, uvFlat, 0).rgb;
        hitT = hit.t;
        if (g_light.y > 0.5) unblurred = Lin(unblurred);   // (a copy with no sRGB view)
    } else {
        refl = Lin(WaterSky(r));                     // another level's water, or no reflection this frame
    }

    // The outgoing reflected leg meets the same height-dependent air as the city. The bridge then integrates
    // the incoming camera-to-water leg once, after the complete water pixel (including foam and reflection).
    bool fogSky = skyward || hitT >= 2999;
    refl = ApplyAtmosphere(refl, p, r, fogSky ? g_atmosphereInfo.y : hitT, false, fogSky);

    // Opaque water: the hidden scene is never a bed or transmitted colour. A slab of the same vertical depth
    // everywhere gives the absorption/scatter distance along the refracted eye ray, independent of scene geometry.
    float3 inWater = refract(-v, s.normal, 1 / 1.333);
    float path = under / max(-inWater.y, 0.05);
    // The opaque slab receives the world's shadow at its surface. Sampling the real shadow map down inside the
    // virtual slab would let submerged geometry draw silhouettes in an otherwise scene-independent body.
    float3 extinct = g_absorb.rgb + scatterB;
    float3 light = WaterGameLight(shadow);
    float3 body = WaterBody(0, path, under, g_absorb.rgb + scatterB * (1 - g_optics.y), g_scatter.rgb, light);
    float3 sunIn = WaterSunWhite() * shadow *
                   WaterSunScatter(v, s.normal, s.slope, sunSeen, under, g_absorb.rgb, scatterB, g_optics.y, s.height,
                                   g_optics.w, g_optics.z);
    // the sky and the sun seen through the crests of steep waves (what enters is the body's share, 1 - Fresnel)
    float3 crestSeen = g_optics.z * WaterThroughCrest(v, s.normal, s.slope, sunSeen, s.height, g_optics.w, extinct, s.var);
    sunIn += crestSeen;
    body += sunIn;
    // A uniformly deep slab has no depth-derived shore band; foam comes from breaking crests alone.
    float foam = saturate(g_scatter.a * WaterWhitecaps(s.jacobian, p.xz, g_water.w));

    // together, in the world's fog
    float fog = FogAmount(z);
    body = lerp(body, light * 0.8, foam);
    body = lerp(body, Lin(g_fogColor.rgb), fog);
    float3 col = lerp(body, refl, fresnel * (1 - foam));
    int debug = (int)g_light.z;
    if (debug == 1) col = refl;
    else if (debug == 2) col = Lin(s.normal * 0.5 + 0.5);
    else if (debug == 3) col = body;
    else if (debug == 4) col = fresnel;
    else if (debug == 5) col = Lin(saturate(sqrt(s.var.xyy) * 4));
    else if (debug == 6) col = unblurred;
    else if (debug == 7) col = sunIn * 8;
    else if (debug == 8) col = shadow;
    else if (debug == 9) col = Lin(saturate(float3(1 - s.jacobian, s.jacobian, 0)));
    else if (debug == 10) col = flatRefl;   // (the reflection where flat water would show it)
    else if (debug == 11) col = saturate(float3(hitT / 20, hitT / 100, hitT >= 2999 ? 1 : 0));   // (how far the reflected ray went)
    else if (debug == 12) col = Lin(saturate(float3(g_reflDepth.SampleLevel(g_point, uvFlat, 0), dot(float4(p, 1), g_rdepth), 0) / 100));
    else if (debug == 13) col = Lin(saturate(float3(abs(s.slope) * 10, sqrt(s.var.x) * 10)));   // (the slopes, x10)
    WaterPixel o;
    o.color = float4(Enc(col), 1);
    float2 motion = 0;
    if (g_temporal.z > 0.5 && i.previous.w > 1e-5) {
        float2 previousPixel = (i.previous.xy / i.previous.w * float2(0.5, -0.5) + 0.5) * g_size.xy;
        motion = previousPixel - (px - g_temporal.xy);   // unjittered, current -> previous, in render pixels
    }
    o.temporal = float4(z, motion, 1);
    return o;
}
