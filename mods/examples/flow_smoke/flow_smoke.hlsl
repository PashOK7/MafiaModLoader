// flow_smoke's drawing (flow_smoke.cpp's graphics pass, Direct3D 11, shader model 5). NVIDIA Flow ray-marches the gas
// in the 64-bit half over the frame it is given: PsPrepare gives it the frame (into the shared textures "color_in" and
// "depth_in"), PsComposite puts what came back ("color_out") on the screen.
#include "mml/gfx/frame.hlsli"

Texture2D<float4> g_flowOut : register(t3);   // "color_out": the frame with the gas over it, linear

struct Prepared {
    float4 color : SV_Target0;   // the frame, linear (Flow lights and blends in linear terms)
    float depth : SV_Target1;    // its depth as the game's projection makes it: _33 + _43 / z (Flow's ray stops there)
};

Prepared PsPrepare(float4 pos : SV_Position) {
    Prepared o;
    o.color = float4(Lin(g_sceneColor.Load(int3(pos.xy, 0)).rgb), 1);
    float z = SceneDepth(pos.xy);
    o.depth = IsSky(z) ? 1 : saturate(g_depthProj.x + g_depthProj.y / max(z, 1e-3));
    return o;
}

// The frame as Flow left it, back into the game's gamma. Fire is brighter than white in linear terms (its colour map
// goes to 78): above 1 its hue is kept and goes towards white the brighter it is (a white-yellow core, an orange
// edge), instead of each channel clipped apart; the frame's own colours (at most 1) are untouched.
float3 Bright(float3 c) {
    float m = max(c.r, max(c.g, c.b));
    return m > 1 ? lerp(c / m, 1, saturate((m - 1) / (m + 4))) : c;
}

float4 PsComposite(float4 pos : SV_Position) : SV_Target {
    float3 c = g_flowOut.Load(int3(pos.xy, 0)).rgb;
    return float4(Enc(Bright(c)), 1);   // (alpha 1: it is the frame here, the gas and what is behind it)
}
