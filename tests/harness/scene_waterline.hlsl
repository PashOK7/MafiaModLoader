#include "mml/gfx/waterline.hlsli"
StructuredBuffer<float4> g_points:register(t0);
RWStructuredBuffer<float4> g_results:register(u0);
[numthreads(1,1,1)]void CsWaterlineTest(uint3 id:SV_DispatchThreadID){
    float4 position=g_points[id.x];g_results[id.x]=float4(MmlQueryWaterline(position.xyz,position.w),0,0);
}
