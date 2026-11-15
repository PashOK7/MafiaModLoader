#include "mml/gfx/ray_scene.hlsli"
struct MmlDxrBounds{float3 lo,hi;};
RWStructuredBuffer<MmlDxrBounds> mml_dxrBounds:register(u0);
cbuffer MmlDxrBuild:register(b0){uint mml_dxrCount;uint3 mml_dxrReserved;};
[numthreads(64,1,1)]
void CsSceneBounds(uint3 id:SV_DispatchThreadID){
    if(id.x>=mml_dxrCount)return;
    MmlRayInstance object=mml_rayInstances[id.x];
    float extent=max(1,max(max(max(abs(object.lo.x),abs(object.lo.y)),abs(object.lo.z)),
                           max(max(abs(object.hi.x),abs(object.hi.y)),abs(object.hi.z))));
    float pad=max(.02,16*1.192092896e-7*extent);
    MmlDxrBounds bounds;bounds.lo=object.lo-pad;bounds.hi=object.hi+pad;mml_dxrBounds[id.x]=bounds;
}
