#include "mml/gfx/frame.hlsli"
StructuredBuffer<float4> cases : register(t4);
RWStructuredBuffer<float4> answers : register(u0);
[numthreads(1,1,1)]
void CsAtmosphereTest(uint id : SV_DispatchThreadID) {
    float4 c = cases[id];
    float dy = (c.y - c.x) / c.z;
    float3 ray = float3(sqrt(saturate(1 - dy * dy)), dy, 0);
    float integral = AtmosphereIntegral(c.x, c.y, c.z, 0, c.w);
    float opacity = AtmosphereOpacity(float3(0,c.x,0), ray, c.z, false, false);
    float sky = AtmosphereOpacity(float3(0,c.x,0), ray, c.z, false, true);
    answers[id] = float4(integral, opacity, sky, 1);
}
