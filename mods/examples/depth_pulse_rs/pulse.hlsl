// depth_pulse_rs's pass (src/lib.rs): a scanner's wave over the world's surfaces, from the frame's depth.
#include "mml/gfx/frame.hlsli"

cbuffer Pulse : register(b1) {
    float4 g_center;   // xyz: where the wave started (world), w: its radius now (metres)
    float4 g_color;    // rgb: its colour, a: its strength
    float4 g_shape;    // x: the ring's width (metres), y: the radius it fades out at, z: the grid's cell (metres)
};

float4 PsPulse(float4 pos : SV_Position) : SV_Target {
    float z = SceneDepth(pos.xy);
    if (IsSky(z)) return 0;
    float3 p = g_eye.xyz + ViewRay(pos.xy) * z;          // the world point this pixel shows
    float d = distance(p.xz, g_center.xz);
    float x = (d - g_center.w) / g_shape.x;
    float ring = exp(-x * x);                             // the wave's front
    float trail = d < g_center.w ? 0.3 * exp((d - g_center.w) / (g_shape.x * 8)) : 0;   // what it has passed
    float2 cell = abs(frac(p.xz / g_shape.z) - 0.5);
    float grid = min(cell.x, cell.y) < 0.025 ? 1.5 : 0;   // lines on the ground behind the front
    float fade = 1 - saturate(g_center.w / g_shape.y);
    float a = saturate((ring + trail * (1 + grid)) * g_color.a * fade) * (1 - FogAmount(z));
    return float4(g_color.rgb * a, a * 0.8);              // premultiplied: the frame keeps 20 % under the light
}
