// flow_smoke's drawing (flow_smoke.cpp's graphics pass, Direct3D 11, shader model 5). NVIDIA Flow ray-marches the gas
// in the 64-bit half: PsPrepare gives it the frame's depth (into the shared texture "depth_in"), Flow marches at the
// size the settings ask (the frame's, or half of it each way) and gives back the gas alone ("color_out": its light,
// premultiplied, linear, and in alpha how much of what is behind it still shows: Flow as patched by
// third_party/flow/patch.py), PsComposite lays it over the frame, brought to the frame's size by the depth.
#include "mml/gfx/frame.hlsli"

Texture2D<float4> g_flowOut : register(t3);   // "color_out" at the march's size: rgb the gas's light, a its transmittance

cbuffer Composite : register(b1) {
    float4 g_march;   // the march's width, height (pixels); the frame's pixels a march pixel across, down (1: the same size)
};

float PsPrepare(float4 pos : SV_Position) : SV_Target0 {
    // the depth as the game's projection makes it: _33 + _43 / z (Flow's ray stops there; the sky 1)
    float z = SceneDepth(pos.xy);
    return IsSky(z) ? 1 : saturate(g_depthProj.x + g_depthProj.y / max(z, 1e-3));
}

// Fire is brighter than white in linear terms (its colour map goes to 78): above 1 its hue is kept and goes towards
// white the brighter it is (a white-yellow core, an orange edge), instead of each channel clipped apart; the frame's
// own colours (at most 1) are untouched.
float3 Bright(float3 c) {
    float m = max(c.r, max(c.g, c.b));
    return m > 1 ? lerp(c / m, 1, saturate((m - 1) / (m + 4))) : c;
}

// The gas at this pixel from the march's pixels around it: each of the four nearest weighted by its distance and by
// how near its depth is to this pixel's (a march pixel looked through to a wall 30 m behind does not spill onto a
// post 2 m in front of it, nor the reverse). A march pixel's depth is the frame's at the pixel Flow took for it
// (Flow's RayMarchBlockCS: floor((i + 0.5) * frame / march)).
float4 GasNear(float2 pixel) {
    const float2 k = g_march.zw;
    const float z = SceneDepth(pixel);
    const float2 at = pixel / k - 0.5;   // (pixel is a centre, x.5: the march's continuous coordinate, its texels' centres at .0)
    const float2 base = floor(at), f = at - base;
    float4 sum = 0;
    float total = 0, nearest = 1e30;
    float4 nearestGas = 0;
    [unroll] for (int j = 0; j < 2; j++)
        [unroll] for (int i = 0; i < 2; i++) {
            const int2 q = clamp(int2(base) + int2(i, j), int2(0, 0), int2(g_march.xy) - 1);
            const float2 src = min(floor((float2(q) + 0.5) * k), g_size.xy - 1);
            const float qz = SceneDepth(src + 0.5);
            const float4 g = g_flowOut.Load(int3(q, 0));
            const float d = abs(qz - z) / max(z, 0.1);
            const float w = (i ? f.x : 1 - f.x) * (j ? f.y : 1 - f.y) * exp(-d * d * 400);   // (5 % of the depth: 1/e)
            sum += g * w;
            total += w;
            if (d < nearest) nearest = d, nearestGas = g;
        }
    return total > 1e-4 ? sum / total : nearestGas;
}
float4 Gas(float2 pixel) {
    // (at the frame's size: its own pixel)
    return g_march.z <= 1 && g_march.w <= 1 ? g_flowOut.Load(int3(pixel, 0)) : GasNear(pixel);
}

// Over what the passes before it drew (premultiplied "over", the bridge's effects target): the frame becomes
// Enc(gas + T * Lin(frame)) exactly where nothing else was drawn, and what was drawn before shows through by T.
float4 PsComposite(float4 pos : SV_Position) : SV_Target {
    const float4 g = Gas(pos.xy);
    const float t = saturate(g.a);
    const float3 frame = g_sceneColor.Load(int3(pos.xy, 0)).rgb;
    const float3 shown = Enc(Bright(g.rgb + t * Lin(frame)));
    // (Enc is concave and Enc(0) is 0, so shown >= t * frame: no negative light)
    return float4(max(shown - t * frame, 0), 1 - t);
}
