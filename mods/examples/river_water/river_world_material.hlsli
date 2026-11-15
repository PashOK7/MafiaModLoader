// Optional world-path providers. Plain SDK geometry/material diagnostics retain their zero-footprint
// contract. Candidate opacity and the finally selected layer use the SAME triangle-local gradients.
#ifndef MML_RIVER_WORLD_MATERIAL_HLSLI
#define MML_RIVER_WORLD_MATERIAL_HLSLI
#define MML_NATIVE_TEXTURE_COMPACT 1
#define MML_RAY_CACHE_SLABS 1
#include "mml/gfx/ray_footprint.hlsli"
#include "mml/gfx/native_texture_footprint.hlsli"
static MmlRayFootprint g_worldFootprint;
static float3 g_worldRayOrigin,g_worldRayDirection;
static float4 g_worldBaryGradient;
static uint g_worldSelectedInstance;
static bool g_worldFootprintValid=false;
static bool g_worldMaterialUnknown=false;
#define MML_SCENE_STAGE_LOOP [loop]
void WaterWorldHitContext(uint triangleIndex,float distance);
float4 WaterWorldTexture(uint surfaceIndex,uint textureId,float2 uv,uint coordinate,uint2 address,
                         uint minFilter,uint magFilter,uint mipFilter,uint maxAnisotropy,uint maxMipLevel,float lodBias,inout bool valid);
#define MML_RAY_OBSERVER(origin) g_worldFootprint.observer
#define MML_RAY_SELECTED_INSTANCE_INDEX(index) g_worldSelectedInstance=index
#define MML_SCENE_LAYER_CONTEXT(hit) WaterWorldHitContext(hit.triangleIndex,hit.distance)
#define MML_SURFACE_TEXTURE(primitive,state,uv,lod,valid) WaterWorldTexture(primitive,state.map.x,state.map.y==0?uv.xy:uv.zw,state.map.y,state.sampling.xy,state.sampling.z,state.sampling.w,state.extra.x,state.extra.y,state.extra.z,asfloat(state.extra.w),valid)
#include "mml/gfx/scene_radiance_ordered.hlsli"
void WaterWorldHitContext(uint triangleIndex,float distance){
    MmlRayTriangle tri=mml_rayTriangles[triangleIndex];
    MmlRayInstance instance=mml_rayInstances[g_worldSelectedInstance];float2 dx,dy;
    g_worldFootprintValid=MmlRaySurfaceFootprint(g_worldFootprint,instance.localX,instance.localY,instance.localZ,instance.flags,tri.e1,tri.e2,
        g_worldRayOrigin,g_worldRayDirection,distance,dx,dy);
    g_worldBaryGradient=float4(dx,dy);
}
float4 WaterWorldTexture(uint surfaceIndex,uint textureId,float2 uv,uint coordinate,uint2 address,
                         uint minFilter,uint magFilter,uint mipFilter,uint maxAnisotropy,uint maxMipLevel,float lodBias,inout bool valid){
    if(!g_worldFootprintValid){valid=false;return 0;}
    MmlSceneSurface surface=mml_surfaces[surfaceIndex];
    float2 a=coordinate==0?surface.uv1.xy-surface.uv0.xy:surface.uv1.zw-surface.uv0.zw;
    float2 b=coordinate==0?surface.uv2.xy-surface.uv0.xy:surface.uv2.zw-surface.uv0.zw;
    float2 dx=a*g_worldBaryGradient.x+b*g_worldBaryGradient.y,dy=a*g_worldBaryGradient.z+b*g_worldBaryGradient.w;
    return MmlTextureSampleFootprintState(textureId,uv,dx,dy,address,minFilter,magFilter,mipFilter,maxAnisotropy,maxMipLevel,lodBias,valid);
}

#endif
