// Path differentials in world metres / direction per pixel. The retained facet quadrature integrates
// unresolved wave slopes separately; adding roughness to these gradients would filter those slopes twice. The
// local triangle intersection, not a screen image or one global mip, determines each UV footprint.
#ifndef MML_RAY_FOOTPRINT_HLSLI
#define MML_RAY_FOOTPRINT_HLSLI
#include "mml/gfx/billboard.hlsli"
struct MmlRayFootprint {
    float3 originDx,originDy,directionDx,directionDy;
    float3 observer,observerDx,observerDy;
};
float3 MmlFootprintLocal(float4 x,float4 y,float4 z,float3 value,float w){
    return float3(dot(x,float4(value,w)),dot(y,float4(value,w)),dot(z,float4(value,w)));
}
bool MmlFootprintRay(float4 x,float4 y,float4 z,uint flags,float3 observer,float3 origin,float3 direction,
                     out float3 localOrigin,out float3 localDirection){
    localOrigin=MmlFootprintLocal(x,y,z,origin,1);localDirection=MmlFootprintLocal(x,y,z,direction,0);
    if((flags&6u)==0)return true;
    float3x3 rotation;bool facing=(flags&2u)!=0;
    if(!MmlBillboardRotation(MmlFootprintLocal(x,y,z,observer,1),facing?2u:1u,facing?0u:1u,rotation))return false;
    localOrigin=mul(rotation,localOrigin);localDirection=mul(rotation,localDirection);return true;
}
bool MmlFootprintBary(float3 edge1,float3 edge2,float3 direction,float distance,float3 deltaOrigin,float3 deltaDirection,out float2 gradient){
    float3 n=cross(edge1,edge2);float denominator=dot(n,direction);
    float determinant=dot(n,n);
    gradient=0;if(abs(denominator)<1e-30||determinant<=0)return false;
    float3 delta=deltaOrigin+distance*deltaDirection;delta-=direction*(dot(n,delta)/denominator);
    gradient=float2(dot(cross(delta,edge2),n),dot(cross(edge1,delta),n))/determinant;
    return !any((asuint(gradient)&0x7f800000u)==0x7f800000u);
}
bool MmlRaySurfaceFootprint(MmlRayFootprint footprint,float4 x,float4 y,float4 z,uint flags,
                            float3 edge1,float3 edge2,float3 origin,float3 direction,float distance,
                            out float2 dx,out float2 dy){
    dx=dy=0;float3 o,d,ox,dxRay,oy,dyRay;
    if(!MmlFootprintRay(x,y,z,flags,footprint.observer,origin,direction,o,d)||
       !MmlFootprintRay(x,y,z,flags,footprint.observer+footprint.observerDx,origin+footprint.originDx,direction+footprint.directionDx,ox,dxRay)||
       !MmlFootprintRay(x,y,z,flags,footprint.observer+footprint.observerDy,origin+footprint.originDy,direction+footprint.directionDy,oy,dyRay))return false;
    return MmlFootprintBary(edge1,edge2,d,distance,ox-o,dxRay-d,dx)&&
           MmlFootprintBary(edge1,edge2,d,distance,oy-o,dyRay-d,dy);
}
#endif
