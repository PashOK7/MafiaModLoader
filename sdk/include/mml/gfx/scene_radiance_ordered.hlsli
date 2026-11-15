// Geometry first, material once for the selected hit. A strict (distance, descending
// draw ID, primitive ID) cursor enumerates rejected alpha surfaces without an epsilon
// tMin jump or a fixed alpha-rejection budget. No texture decoder lives inside BLAS/TLAS.
#ifndef MML_SCENE_RADIANCE_ORDERED_HLSLI
#define MML_SCENE_RADIANCE_ORDERED_HLSLI
#include "mml/gfx/scene_surface.hlsli"
static bool mml_orderCursor=false;
static float mml_orderDistance=0;
static uint mml_orderInstance=0,mml_orderPrimitive=0;
uint MmlOrderedAccept(uint primitive,uint instanceId,float distance);
bool MmlOrderedWins(float distance,uint instanceId,uint primitive,float currentDistance,uint currentInstance,uint currentPrimitive);
#define MML_RAY_ACCEPT_PRIMITIVE(triangleIndex,primitive,bary,instanceId,distance) MmlOrderedAccept(primitive,instanceId,distance)
#define MML_RAY_WINS(distance,instanceId,primitive,currentDistance,currentInstance,currentPrimitive) MmlOrderedWins(distance,instanceId,primitive,currentDistance,currentInstance,currentPrimitive)
#include "mml/gfx/ray_scene.hlsli"
uint MmlOrderedAccept(uint primitive,uint instanceId,float distance){
    if(mml_orderCursor){
        if(distance<mml_orderDistance)return 0;
        if(distance==mml_orderDistance&&(instanceId>mml_orderInstance||
           (instanceId==mml_orderInstance&&primitive<=mml_orderPrimitive)))return 0;
    }
    return 1;
}
bool MmlOrderedWins(float distance,uint instanceId,uint primitive,float currentDistance,uint currentInstance,uint currentPrimitive){
    return currentInstance==0xffffffff||distance<currentDistance||
        (distance==currentDistance&&(instanceId>currentInstance||(instanceId==currentInstance&&primitive<currentPrimitive)));
}
#ifndef MML_SCENE_LAYER_LIGHT
#define MML_SCENE_LAYER_LIGHT(color,origin,direction,distance) color
#endif
#ifndef MML_SCENE_LAYER_LOOP
#define MML_SCENE_LAYER_LOOP [loop]
#endif
#ifndef MML_SCENE_LAYER_CONTEXT
#define MML_SCENE_LAYER_CONTEXT(hit)
#endif
struct MmlSceneRadiance {MmlRayHit first;float3 color;float transmission;uint status,layers;float2 pad;};
MmlSceneRadiance MmlTraceSceneRadiance(float3 origin,float3 direction,float tMin,float tMax,float lod){
    MmlSceneRadiance result=(MmlSceneRadiance)0;result.transmission=1;
    mml_orderCursor=false;uint visited=0;
    // The layer history is LOCAL to this radiance query. Carrying a mutable
    // global array through the outer optical/wave loops explodes FXC's SSA graph.
    uint draws[32];float depths[32];
    float precision=4*1.192092896e-7*max(1,max(max(abs(origin.x),abs(origin.y)),max(abs(origin.z),tMax)));
    // Only contributing layers consume the retained32-layer bound. A rejected cutout
    // advances the exact geometric cursor, never discards other draws at the same depth.
    MML_SCENE_LAYER_LOOP while(result.layers<32){
        // The preceding query proved the segment before its hit complete.
        // Keep a roundoff margin in the slab range. The separate exact
        // cursor still decides which coplanar triangles may contribute.
        float begin=mml_orderCursor?max(tMin,mml_orderDistance-precision):tMin;
        MmlRayHit hit=MmlTraceWorld(origin,direction,begin,tMax);
        if(hit.status!=1){if(result.layers==0)result.first=hit;result.status=hit.status;return result;}
        mml_orderCursor=true;mml_orderDistance=hit.distance;
        mml_orderInstance=hit.instance;mml_orderPrimitive=hit.primitive;
        // Deduplicate native draws outside traversal as well. BLAS/TLAS carries
        // only the four-scalar cursor. Only earlier contributing layers have history entries.
        bool duplicate=false;
        [branch]if(visited!=0)
            [loop]for(uint i=0;i<32;i++){
                if(i>=visited)break;
                duplicate=duplicate||(draws[i]==hit.instance&&abs(depths[i]-hit.distance)<=precision);
            }
        if(duplicate)continue;
        MML_SCENE_LAYER_CONTEXT(hit);
        float4 encoded;uint material;
        // Compact attributes follow physical BLAS triangle order. The
        // geometric cursor above still uses the original per-draw primitive.
        bool compact=mml_rayCoverageLo.w!=0;
        uint surface=compact?hit.triangleIndex:hit.primitive;
        if(!MmlSurfaceEncodedAt(surface,hit.bary,lod,compact?hit.material:0xffffffff,encoded,material)){
            if(result.layers==0){result.first=hit;result.first.status=2;}result.status=2;return result;
        }
        MmlSceneMaterial m=mml_materials[material];
        if(m.info.x!=0){
            if(m.info.z<1||m.info.z>8){result.status=2;return result;}
            if(!MmlNativeAlphaPass(m.info.z,encoded.a,m.info.y))continue;
        }
        if(m.blend.x!=0&&(m.blend.y!=5||(m.blend.z!=2&&m.blend.z!=6))){result.status=2;return result;}
        if(result.layers==0)result.first=hit;
        float weight=m.blend.x==0?1:encoded.a;
        result.color+=result.transmission*weight*MML_SCENE_LAYER_LIGHT(MmlNativeLinear(encoded.rgb),origin,direction,hit.distance);
        result.layers++;
        if(m.blend.x==0){result.transmission=0;result.status=1;return result;}
        if(m.blend.z==6)result.transmission*=1-encoded.a;
        if(result.transmission==0){result.status=1;return result;}
        // Initialize FXC's indexable temporaries only when a transparent layer needs history.
        [branch]if(visited==0)
            [loop]for(uint init=0;init<32;init++){draws[init]=0xffffffff;depths[init]=-1;}
        draws[visited]=hit.instance;depths[visited]=hit.distance;visited++;
    }
    result.status=2;return result;
}
#endif
