#include "mml/gfx/billboard.hlsli"
struct Probe {float4 observer;float4 world0,world1,world2,world3;uint4 info;};
StructuredBuffer<Probe> probes:register(t0);
RWStructuredBuffer<float4> results:register(u0);
[numthreads(1,1,1)]void CsBillboard(uint3 id:SV_DispatchThreadID){
    Probe p=probes[id.x];float3x3 rotation;bool valid=MmlBillboardRotation(p.observer.xyz,p.info.x,p.info.y,rotation);
    [unroll]for(uint row=0;row<3;row++)results[id.x*5+row]=rotation[row].x*p.world0+rotation[row].y*p.world1+rotation[row].z*p.world2;
    results[id.x*5+3]=p.world3;results[id.x*5+4]=valid?1:0;
}
