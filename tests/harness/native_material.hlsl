#include "mml/gfx/native_material.hlsli"
#include "mml/gfx/scene_surface.hlsli"
struct Probe {float4 tex0,tex1,diffuse,factor;uint4 ops,info;};
StructuredBuffer<Probe> probes:register(t0);
RWStructuredBuffer<float4> output:register(u0);
[numthreads(64,1,1)]void CsNativeMaterial(uint3 id:SV_DispatchThreadID){
    uint count,stride;probes.GetDimensions(count,stride);if(id.x>=count)return;
    Probe p=probes[id.x];bool valid=true;
    float4 c=MmlNativeStage(uint4(p.ops.x,1,2,0),uint4(p.ops.z,1,2,0),p.tex0,p.diffuse,p.diffuse,p.factor,p.ops.w!=0,valid);
    if(p.ops.w!=0 && p.ops.y!=1)c=MmlNativeStage(uint4(p.ops.y,1,2,1),uint4(4,1,2,1),p.tex1,c,p.diffuse,p.factor,true,valid);
    bool accepted=p.info.x==0||MmlNativeAlphaPass(p.info.y,c.a,p.info.z);
    output[id.x*2]=c;output[id.x*2+1]=float4(MmlNativeLinear(c.rgb),valid?(accepted?1:0):-1);
}
