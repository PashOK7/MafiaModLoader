// D3D11 world-geometry query core. The caller binds an immutable mesh BLAS and a SAME-FRAME instance/TLAS
// snapshot. Materials supply MML_RAY_ACCEPT(triangleIndex,barycentrics): 0 transparent, 1 opaque, 2 unknown.
// Missing alpha/material data is unknown, never a transparent sky miss. No camera or projected image inputs.
#ifndef MML_RAY_SCENE_HLSLI
#define MML_RAY_SCENE_HLSLI
#include "mml/gfx/billboard.hlsli"
struct MmlRayTriangle{float3 p;uint material;float3 e1;uint primitive;float3 e2;uint flags;};
struct MmlRayNode{float3 lo;uint first;float3 hi;uint count;uint escape;uint3 pad;};
struct MmlRayInstance{float4 localX,localY,localZ;uint root,id,flags,pad;float3 lo;uint pad1;float3 hi;uint pad2;};
struct MmlRayUnknown{float3 lo;uint reason;float3 hi;uint pad;};
StructuredBuffer<MmlRayNode> mml_rayNodes : register(t16);
StructuredBuffer<MmlRayTriangle> mml_rayTriangles : register(t17);
StructuredBuffer<MmlRayNode> mml_rayTop : register(t18);
StructuredBuffer<MmlRayInstance> mml_rayInstances : register(t19);
StructuredBuffer<uint> mml_rayOrder : register(t20);
StructuredBuffer<MmlRayUnknown> mml_rayUnknown : register(t21);
// xyzw: BLAS nodes, triangles, TLAS nodes, instance count. xy: instance-order count, unknown-box count.
cbuffer MmlRaySnapshot : register(b3){uint4 mml_rayCounts;uint4 mml_rayMore;float4 mml_rayCoverageLo,mml_rayCoverageHi;};
#ifndef MML_RAY_ACCEPT
#define MML_RAY_ACCEPT(triangleIndex,barycentrics) ((mml_rayTriangles[triangleIndex].flags&1)!=0?1u:2u)
#endif
#ifndef MML_RAY_ACCEPT_HIT
#define MML_RAY_ACCEPT_HIT(triangleIndex,barycentrics,instanceId,distance) MML_RAY_ACCEPT(triangleIndex,barycentrics)
#endif
#ifndef MML_RAY_ACCEPT_PRIMITIVE
#define MML_RAY_ACCEPT_PRIMITIVE(triangleIndex,primitive,barycentrics,instanceId,distance) MML_RAY_ACCEPT_HIT(triangleIndex,barycentrics,instanceId,distance)
#endif
#ifndef MML_RAY_WINS
#define MML_RAY_WINS(distance,instanceId,primitiveId,currentDistance,currentInstance,currentPrimitive) true
#endif
#ifndef MML_RAY_OBSERVER
#define MML_RAY_OBSERVER(origin) origin
#endif
#ifndef MML_RAY_HIT_CONTEXT
#define MML_RAY_HIT_CONTEXT(object,origin,direction,index,distance)
#endif
#ifndef MML_RAY_SELECTED_CONTEXT
#define MML_RAY_SELECTED_CONTEXT(hit)
#endif
#ifndef MML_RAY_FINISH_CONTEXT
#define MML_RAY_FINISH_CONTEXT(hit)
#endif
#ifndef MML_RAY_SELECTED_INSTANCE_INDEX
#define MML_RAY_SELECTED_INSTANCE_INDEX(index)
#endif
#ifndef MML_RAY_TRAVERSAL_LOOP
#define MML_RAY_TRAVERSAL_LOOP [loop]
#endif
// Explicit parallel slabs avoid 0*infinity NaNs at axis-aligned boundaries.
bool MmlRayBox(float3 origin,float3 direction,float3 lo,float3 hi,float tMin,float tMax,out float entry){
    entry=tMin;float leave=tMax;
    [unroll]for(uint axis=0;axis<3;axis++){
        if(direction[axis]==0){if(origin[axis]<lo[axis]||origin[axis]>hi[axis])return false;}
        else{float a=(lo[axis]-origin[axis])/direction[axis],b=(hi[axis]-origin[axis])/direction[axis];entry=max(entry,min(a,b));leave=min(leave,max(a,b));}
    }
    return entry<=leave;
}
#if defined(MML_RAY_CACHE_SLABS)
struct MmlRaySlabs {float3 inverse;bool3 parallel;bool usable;};
MmlRaySlabs MmlPrepareSlabs(float3 direction){
    MmlRaySlabs slabs;
    slabs.parallel=direction==0;
    slabs.inverse=rcp(slabs.parallel?1:direction);
    slabs.usable=!any((asuint(slabs.inverse)&0x7f800000u)==0x7f800000u)&&
                 !any(!slabs.parallel&&slabs.inverse==0);
    return slabs;
}
bool MmlRayNodeBox(float3 origin,MmlRaySlabs slabs,float3 lo,float3 hi,float tMin,float tMax,float roundoff){
    // The approximate reciprocal only decides whether to visit known
    // geometry. Boundary visits are conservative; triangle t is unchanged.
    if(!slabs.usable)return true; // extreme direction: retain the candidate rather than cull it
    if(any(slabs.parallel&&((origin<lo)||(origin>hi))))return false;
    float3 a=(lo-origin)*slabs.inverse,b=(hi-origin)*slabs.inverse;
    float3 first=slabs.parallel?tMin:min(a,b),last=slabs.parallel?tMax:max(a,b);
    float entry=max(tMin,max(first.x,max(first.y,first.z)));
    float leave=min(tMax,min(last.x,min(last.y,last.z)));
    return entry<=leave+roundoff;
}
#endif
bool MmlRayTriangleHit(MmlRayTriangle tri,float3 origin,float3 direction,float tMin,float tMax,bool mirrored,out float t,out float2 bary){
    float3 p=cross(direction,tri.e2);float determinant=dot(tri.e1,p);t=tMax;bary=0;
    float facing=mirrored?-determinant:determinant;
    if(determinant==0 || ((tri.flags&2)!=0&&facing<=0) || ((tri.flags&4)!=0&&facing>=0))return false;
    float inverse=1/determinant;float3 s=origin-tri.p;
    float u=dot(s,p)*inverse;if(u<0||u>1)return false;
    float3 q=cross(s,tri.e1);float v=dot(direction,q)*inverse;if(v<0||u+v>1)return false;
    t=dot(tri.e2,q)*inverse;bary=float2(u,v);return t>=tMin&&t<=tMax;
}
// status: 0 confirmed miss of this COMPLETE bounded segment, 1 hit, 2 incomplete/unknown, 3 invalid input.
// A ray leaving covered geometry before its requested end remains UNKNOWN even if an image had sky there.
struct MmlRayHit{uint status,instance,triangleIndex,material;float distance;float2 bary;uint primitive;};
bool MmlRayFinite(float value){return (asuint(value)&0x7f800000u)!=0x7f800000u;}
bool MmlRayReverse(MmlRayNode root,float3 direction){
#if defined(MML_RAY_DIRECTIONAL_ORDER)
    // Both orderings traverse the same tree. The ordered material consumer
    // resolves distance ties independently of which child was visited first.
    return root.pad.y!=0&&direction[root.pad.z]<0;
#else
    return false;
#endif
}
#if defined(MML_RAY_DXR)
#include "mml/gfx/ray_scene_dxr.hlsli"
#endif
MmlRayHit MmlTraceWorld(float3 origin,float3 direction,float tMin,float tMax){
    MmlRayHit hit;hit.status=0;hit.instance=hit.triangleIndex=hit.material=hit.primitive=0xffffffff;hit.distance=tMax;hit.bary=0;
    if(!MmlRayFinite(origin.x)||!MmlRayFinite(origin.y)||!MmlRayFinite(origin.z)||
       !MmlRayFinite(direction.x)||!MmlRayFinite(direction.y)||!MmlRayFinite(direction.z)||
       !MmlRayFinite(tMin)||!MmlRayFinite(tMax)||tMin<0||tMax<tMin||dot(direction,direction)<1e-20){hit.status=3;return hit;}
    float unknownAt=asfloat(0x7f800000u); // infinity sentinel: tMax+1 can round back to tMax for a long segment
    float3 begin=origin+direction*tMin,end=origin+direction*tMax;
    if(any(begin<mml_rayCoverageLo.xyz)||any(begin>mml_rayCoverageHi.xyz))unknownAt=tMin;
    else if(any(end<mml_rayCoverageLo.xyz)||any(end>mml_rayCoverageHi.xyz)){
        float leave=tMax;
        [unroll]for(uint axis=0;axis<3;axis++)if(direction[axis]!=0){float bound=direction[axis]>0?mml_rayCoverageHi[axis]:mml_rayCoverageLo[axis];leave=min(leave,(bound-origin[axis])/direction[axis]);}
        unknownAt=max(leave,tMin);
    }
    [loop]for(uint missing=0;missing<mml_rayMore.y;missing++){
        MmlRayUnknown box=mml_rayUnknown[missing];float entry;
        if(MmlRayBox(origin,direction,box.lo,box.hi,tMin,tMax,entry))unknownAt=min(unknownAt,entry);
    }
#if defined(MML_RAY_DXR)
    MmlDxrWorld(origin,direction,tMin,tMax,hit,unknownAt);
#else
#if defined(MML_RAY_CACHE_SLABS)
    MmlRaySlabs worldSlabs=MmlPrepareSlabs(direction);
    float roundoff=8*1.192092896e-7*max(1,tMax);
#endif
    uint top=0;
    bool reverseTop=false;
#if defined(MML_RAY_DIRECTIONAL_ORDER)
    if(mml_rayCounts.z)reverseTop=MmlRayReverse(mml_rayTop[0],direction);
#endif
    MML_RAY_TRAVERSAL_LOOP while(top<mml_rayCounts.z){
        MmlRayNode node=mml_rayTop[top];float entry;
#if defined(MML_RAY_CACHE_SLABS)
        if(!MmlRayNodeBox(origin,worldSlabs,node.lo,node.hi,tMin,hit.distance,roundoff)){top=reverseTop?node.pad.x:node.escape;continue;}
#else
        if(!MmlRayBox(origin,direction,node.lo,node.hi,tMin,hit.distance,entry)){top=reverseTop?node.pad.x:node.escape;continue;}
#endif
        if(node.count==0){top=reverseTop?node.pad.y:node.first;continue;}
        MML_RAY_TRAVERSAL_LOOP for(uint objectIndex=0;objectIndex<node.count;objectIndex++){
            uint instanceIndex=mml_rayOrder[node.first+objectIndex];
            MmlRayInstance object=mml_rayInstances[instanceIndex];
#if defined(MML_RAY_CACHE_SLABS)
            if(!MmlRayNodeBox(origin,worldSlabs,object.lo,object.hi,tMin,hit.distance,roundoff))continue;
#else
            if(!MmlRayBox(origin,direction,object.lo,object.hi,tMin,hit.distance,entry))continue;
#endif
            float3 localOrigin=float3(dot(object.localX,float4(origin,1)),dot(object.localY,float4(origin,1)),dot(object.localZ,float4(origin,1)));
            float3 localDirection=float3(dot(object.localX.xyz,direction),dot(object.localY.xyz,direction),dot(object.localZ.xyz,direction));
            if((object.flags&6)!=0){
                float3x3 rotation;bool facing=(object.flags&2)!=0;
                float3 observer=MML_RAY_OBSERVER(origin);
                float3 localObserver=float3(dot(object.localX,float4(observer,1)),dot(object.localY,float4(observer,1)),dot(object.localZ,float4(observer,1)));
                if(!MmlBillboardRotation(localObserver,facing?2u:1u,facing?0u:1u,rotation)){
#if defined(MML_RAY_CACHE_SLABS)
                    if(MmlRayBox(origin,direction,object.lo,object.hi,tMin,hit.distance,entry))
#endif
                        unknownAt=min(unknownAt,entry);
                    continue;
                }
                localOrigin=mul(rotation,localOrigin);localDirection=mul(rotation,localDirection);
            }
#if defined(MML_RAY_CACHE_SLABS)
            MmlRaySlabs localSlabs=MmlPrepareSlabs(localDirection);
#endif
            uint meshNode=object.root;
            MmlRayNode root=mml_rayNodes[meshNode];
            uint stop=root.escape;
            bool reverseMesh=MmlRayReverse(root,localDirection);
            MML_RAY_TRAVERSAL_LOOP while(meshNode<stop){
                MmlRayNode mesh=mml_rayNodes[meshNode];
#if defined(MML_RAY_CACHE_SLABS)
                if(!MmlRayNodeBox(localOrigin,localSlabs,mesh.lo,mesh.hi,tMin,hit.distance,roundoff)){meshNode=reverseMesh?mesh.pad.x:mesh.escape;continue;}
#else
                float boxEntry;
                if(!MmlRayBox(localOrigin,localDirection,mesh.lo,mesh.hi,tMin,hit.distance,boxEntry)){meshNode=reverseMesh?mesh.pad.x:mesh.escape;continue;}
#endif
                if(mesh.count==0){meshNode=reverseMesh?mesh.pad.y:mesh.first;continue;}
                MML_RAY_TRAVERSAL_LOOP for(uint primitiveIndex=0;primitiveIndex<mesh.count;primitiveIndex++){
                    uint triangleIndex=mesh.first+primitiveIndex;MmlRayTriangle tri=mml_rayTriangles[triangleIndex];float distance;float2 bary;
                    if(!MmlRayTriangleHit(tri,localOrigin,localDirection,tMin,hit.distance,(object.flags&1)!=0,distance,bary))continue;
                    bool instanceAttributes=(object.flags&8)!=0;
                    uint primitive=tri.primitive+(instanceAttributes?object.pad1:0);
                    MML_RAY_HIT_CONTEXT(object,origin,direction,triangleIndex,distance);
                    uint accepted=MML_RAY_ACCEPT_PRIMITIVE(triangleIndex,primitive,bary,object.id,distance);
                    if(accepted==2){unknownAt=min(unknownAt,distance);continue;}
                    if(accepted==0)continue;
                    if(!MML_RAY_WINS(distance,object.id,primitive,hit.distance,hit.instance,hit.primitive))continue;
                    hit.status=1;hit.instance=object.id;hit.triangleIndex=triangleIndex;hit.material=instanceAttributes?object.pad:tri.material;
                    hit.distance=distance;hit.bary=bary;hit.primitive=primitive;
                    MML_RAY_SELECTED_CONTEXT(hit);
                    MML_RAY_SELECTED_INSTANCE_INDEX(instanceIndex);
                }
                meshNode=reverseMesh?mesh.pad.x:mesh.escape;
            }
        }
        top=reverseTop?node.pad.x:node.escape;
    }
#endif
    if(unknownAt<=hit.distance){hit.status=2;hit.distance=unknownAt;hit.instance=hit.triangleIndex=hit.material=hit.primitive=0xffffffff;hit.bary=0;}
    if(hit.status==1){MML_RAY_FINISH_CONTEXT(hit);}
    return hit;
}
#endif
