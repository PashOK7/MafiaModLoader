// The graphics bridge's Direct3D 11 side (host/gfx.cpp), shader model 5: the full-screen vertex shader every pass
// gets bound, from the SDK's frame.hlsli (what mods include).
#include "mml/gfx/frame.hlsli"

// Geometry replacements drawn before the upscaler: the same frame's world and premultiplied effects, in display
// space as the bridge's final Direct3D 9 composite. Keep every pixel at the render-size location (no resampling).
Texture2D<float4> g_preEffects : register(t4);
Texture2D<float4> g_preSurface : register(t5);
float4 PsPreComposite(float4 pos : SV_Position) : SV_Target {
    float4 fx = g_preEffects.Load(int3(pos.xy, 0));
    float3 color = g_sceneColor.Load(int3(pos.xy, 0)).rgb * (1 - fx.a) + fx.rgb;
    if (g_atmosphereInfo.x > 0.5) {
        float surface = g_preSurface.Load(int3(pos.xy, 0)).x;
        float depth = surface > 0 ? surface : SceneDepth(pos.xy);
        bool sky = IsSky(depth);
        float3 ray = ViewRay(pos.xy);
        float distance = sky ? g_atmosphereInfo.y : depth * length(ray);
        color = Enc(ApplyAtmosphere(Lin(color), g_eye.xyz, normalize(ray), distance, true, sky));
    }
    return float4(color, 1);
}
struct TemporalInputs { float depth : SV_Target0; float2 motion : SV_Target1; };
TemporalInputs PsPreTemporal(float4 pos : SV_Position) {
    TemporalInputs o;
    float4 surface = g_preEffects.Load(int3(pos.xy, 0));
    o.depth = surface.x > 0 ? surface.x : g_sceneDepth.Load(int3(pos.xy, 0));
    o.motion = surface.x > 0 ? surface.yz : g_motion.Load(int3(pos.xy, 0));
    return o;
}

// The upscaler's frame (gfx.cpp, gfx_upscale): the world drawn at the render size into the top-left of the frame's
// textures. PsStretch: that picture over the whole frame, filtered (the upscaler drew nothing this frame); PsUpDepth,
// PsUpMotion: the depth and the motion vectors at the frame's size for the passes after the upscaler (the nearest
// render pixel's; the motion in the frame's pixels).
cbuffer Up : register(b1) {
    float4 g_upSize;   // the render size, the frame's size (pixels)
};
float4 PsStretch(float4 pos : SV_Position) : SV_Target {
    float2 at = min(pos.xy * g_upSize.xy / g_upSize.zw, g_upSize.xy - 0.5);
    return float4(g_sceneColor.SampleLevel(g_linear, at / g_upSize.zw, 0).rgb, 1);
}
float PsUpDepth(float4 pos : SV_Position) : SV_Target {
    return g_sceneDepth.Load(int3(min(floor(pos.xy * g_upSize.xy / g_upSize.zw), g_upSize.xy - 1), 0));
}
float2 PsUpMotion(float4 pos : SV_Position) : SV_Target {
    return g_motion.Load(int3(min(floor(pos.xy * g_upSize.xy / g_upSize.zw), g_upSize.xy - 1), 0)) * g_upSize.zw / g_upSize.xy;
}
