#define MML_RAY_DIRECTIONAL_ORDER 1
#include "../../mods/examples/river_water/river_world_material.hlsli"
StructuredBuffer<float4> probes:register(t22);
RWStructuredBuffer<float4> results:register(u0);
[numthreads(4,1,1)]void CsSceneBlend(uint3 id:SV_DispatchThreadID){
    float4 o=probes[id.x*2],d=probes[id.x*2+1];
    g_worldRayOrigin=o.xyz;g_worldRayDirection=d.xyz;
    g_worldFootprint=(MmlRayFootprint)0;g_worldFootprint.observer=o.xyz;
    g_worldFootprint.directionDx=probes[8+id.x].xyz;
    MmlSceneRadiance r=MmlTraceSceneRadiance(o.xyz,d.xyz,o.w,d.w,0);
    results[id.x*2]=float4(r.color,r.transmission);results[id.x*2+1]=float4(r.status,r.layers,r.first.instance,r.first.distance);
}
