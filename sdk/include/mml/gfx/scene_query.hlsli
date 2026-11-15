// Native material world queries and ordered linear-radiance composition. No scene-frame color/depth.
// The inventory requires SRCALPHA/INVSRCALPHA and SRCALPHA/ONE. Unknown state/coverage remains UNKNOWN.
// Up to32 contributing draw layers; exceeding the bound never fabricates a complete sky miss.
#ifndef MML_SCENE_QUERY_HLSLI
#define MML_SCENE_QUERY_HLSLI
#include "mml/gfx/scene_surface.hlsli"
static bool mml_queryMaterials=false,mml_queryBlend=false;
static float mml_queryLod=0,mml_queryPrecision=0;
static uint mml_queryVisited=0;
static uint4 mml_queryInstance[8];
static float4 mml_queryDistance[8];
uint MmlSceneAccept(uint primitive,float2 bary,uint instanceId,float distance);
bool MmlSceneWins(float distance,uint instanceId,uint primitiveId,float currentDistance,uint currentInstance,uint currentPrimitive);
#define MML_RAY_ACCEPT_PRIMITIVE(triangleIndex,primitive,bary,instanceId,distance) MmlSceneAccept(primitive,bary,instanceId,distance)
#define MML_RAY_WINS(distance,instanceId,primitiveId,currentDistance,currentInstance,currentPrimitive) MmlSceneWins(distance,instanceId,primitiveId,currentDistance,currentInstance,currentPrimitive)
#include "mml/gfx/ray_scene.hlsli"
uint MmlSceneAccept(uint primitive,float2 bary,uint instanceId,float distance){
    if(!mml_queryMaterials)return 1u;
    if(mml_queryBlend){
        // One raster draw contributes once at a coplanar/shared triangle edge. Other draws at that same
        // distance still contribute; advancing tMin by an epsilon would silently erase those overlays.
        [unroll]for(uint i=0;i<8;i++){
            uint4 lanes=uint4(i*4,i*4+1,i*4+2,i*4+3);
            bool4 match=(mml_queryInstance[i]==instanceId)&&(abs(mml_queryDistance[i]-distance)<=mml_queryPrecision);
            if(any(match&&(lanes<mml_queryVisited)))return 0u;
        }
        return MmlSurfaceLayer(primitive,bary,mml_queryLod);
    }
    return MmlSurfaceOpacity(primitive,bary,mml_queryLod);
}
bool MmlSceneWins(float distance,uint instanceId,uint primitiveId,float currentDistance,uint currentInstance,uint currentPrimitive){
    if(!mml_queryBlend||currentInstance==0xffffffff||distance<currentDistance)return true;
    // Exact depth ties: later source draws overlay earlier ones. Within one draw, stable primitive ID wins.
    return distance==currentDistance&&(instanceId>currentInstance||(instanceId==currentInstance&&primitiveId<currentPrimitive));
}
MmlRayHit MmlSceneGeometryRay(float3 origin,float3 direction,float tMin,float tMax){
    mml_queryMaterials=false;mml_queryBlend=false;mml_queryVisited=0;return MmlTraceWorld(origin,direction,tMin,tMax);
}
MmlRayHit MmlSceneOpacityRay(float3 origin,float3 direction,float tMin,float tMax,float lod){
    mml_queryMaterials=true;mml_queryBlend=false;mml_queryVisited=0;mml_queryLod=lod;return MmlTraceWorld(origin,direction,tMin,tMax);
}
#ifndef MML_SCENE_LAYER_LIGHT
#define MML_SCENE_LAYER_LIGHT(color,origin,direction,distance) color
#endif
#ifndef MML_SCENE_LAYER_LOOP
#define MML_SCENE_LAYER_LOOP [loop]
#endif
#ifndef MML_SCENE_RADIANCE_ENCODED
#define MML_SCENE_RADIANCE_ENCODED(primitive,bary,lod,color,material) MmlSurfaceEncoded(primitive,bary,lod,color,material)
#endif
struct MmlSceneRadiance {MmlRayHit first;float3 color;float transmission;uint status,layers;float2 pad;};
MmlSceneRadiance MmlTraceSceneRadiance(float3 origin,float3 direction,float tMin,float tMax,float lod){
    MmlSceneRadiance result=(MmlSceneRadiance)0;result.transmission=1;
    mml_queryMaterials=true;mml_queryBlend=true;mml_queryVisited=0;mml_queryLod=lod;
    [unroll]for(uint init=0;init<8;init++){mml_queryInstance[init]=0xffffffff;mml_queryDistance[init]=-1;}
    // Four single-precision world-coordinate ulps define indistinguishable coplanar depth within one draw.
    // This only deduplicates that draw, never another instance/material at the same distance.
    mml_queryPrecision=4*1.192092896e-7*max(1,max(max(abs(origin.x),abs(origin.y)),max(abs(origin.z),tMax)));
    MML_SCENE_LAYER_LOOP for(uint layer=0;layer<32;layer++){
        MmlRayHit hit=MmlTraceWorld(origin,direction,tMin,tMax);
        if(layer==0)result.first=hit;
        if(hit.status!=1){result.status=hit.status;return result;}
        float4 encoded;uint material;
        if(!MML_SCENE_RADIANCE_ENCODED(hit.primitive,hit.bary,lod,encoded,material)){result.status=2;return result;}
        MmlSceneMaterial m=mml_materials[material];float weight=m.blend.x==0?1:encoded.a;
        float3 color=MmlNativeLinear(encoded.rgb);
        result.color+=result.transmission*weight*MML_SCENE_LAYER_LIGHT(color,origin,direction,hit.distance);
        result.layers++;
        if(m.blend.x==0){result.transmission=0;result.status=1;return result;}
        if(m.blend.z==6)result.transmission*=1-encoded.a; // ONE keeps the deeper contribution intact
        if(result.transmission==0){result.status=1;return result;}
        // Constant register indices keep FXC from forcing the whole nested traversal to unroll.
        [unroll]for(uint store=0;store<8;store++){
            uint4 slots=uint4(store*4,store*4+1,store*4+2,store*4+3);bool4 write=slots==mml_queryVisited;
            mml_queryInstance[store]=write?hit.instance:mml_queryInstance[store];
            mml_queryDistance[store]=write?hit.distance:mml_queryDistance[store];
        }mml_queryVisited++;
    }
    result.status=2;return result;
}
#endif
