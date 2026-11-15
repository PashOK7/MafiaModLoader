static bool g_testNativeOpacity=false;
uint TestOpacity(uint triangleIndex,float2 bary);
#define MML_RAY_ACCEPT(triangleIndex,barycentrics) TestOpacity(triangleIndex,barycentrics)
#define MML_RAY_CACHE_SLABS 1
#include "mml/gfx/ray_scene.hlsli"
#include "mml/gfx/scene_surface.hlsli"
#include "mml/gfx/native_texture_pages.hlsli"
uint TestOpacity(uint triangleIndex,float2 bary){
    uint material=mml_rayTriangles[triangleIndex].material;
    uint accepted=material==12?2u:material==11?(bary.x>.3?0u:1u):1u;
    if(g_testNativeOpacity)accepted=MmlSurfaceOpacity(mml_rayTriangles[triangleIndex].primitive,bary,0);
    return accepted;
}
StructuredBuffer<float4> probes:register(t22); // origin/tMin, direction/tMax, two records a ray
RWStructuredBuffer<float4> results:register(u0);
[numthreads(8,1,1)]void CsRaySceneTest(uint3 id:SV_DispatchThreadID){
    float4 o=probes[id.x*2],d=probes[id.x*2+1];MmlRayHit hit=MmlTraceWorld(o.xyz,d.xyz,o.w,d.w);
    results[id.x*2]=float4(hit.status,hit.distance,hit.instance,hit.material);
    results[id.x*2+1]=float4(hit.bary,hit.primitive,hit.triangleIndex);
}
[numthreads(4,1,1)]void CsRayAttributes(uint3 id:SV_DispatchThreadID){
    float4 o=probes[id.x*2],d=probes[id.x*2+1];MmlRayHit hit=MmlTraceWorld(o.xyz,d.xyz,o.w,d.w);
    float4 uv=0,diffuse=0,checker=0;uint material;bool valid=MmlSurfaceAttributes(hit.primitive,hit.bary,uv,diffuse,material);
    if(valid)checker=MmlTextureSample(0,uv.xy,0,uint2(1,1),1,1,0,valid);
    results[id.x*4]=float4(hit.status,hit.instance,material,hit.distance);
    results[id.x*4+1]=uv;results[id.x*4+2]=diffuse;results[id.x*4+3]=checker;
}
[numthreads(4,1,1)]void CsRayMaterials(uint3 id:SV_DispatchThreadID){
    g_testNativeOpacity=probes[8].x>.5;
    float4 o=probes[id.x*2],d=probes[id.x*2+1];MmlRayHit hit=MmlTraceWorld(o.xyz,d.xyz,o.w,d.w);
    float4 encoded;uint material;bool valid=MmlSurfaceEncoded(hit.primitive,hit.bary,0,encoded,material);
    results[id.x*4]=encoded;results[id.x*4+1]=float4(MmlNativeLinear(encoded.rgb),encoded.a);
    results[id.x*4+2]=float4(valid?1:0,hit.instance,material,hit.primitive);results[id.x*4+3]=0;
}
