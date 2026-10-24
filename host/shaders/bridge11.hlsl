// The graphics bridge's Direct3D 11 side (host/gfx.cpp), shader model 5: the full-screen vertex shader every pass
// gets bound, from the SDK's frame.hlsli (what mods include).
#include "mml/gfx/frame.hlsli"

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
