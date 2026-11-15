// Directional environment/medium and current world-object paths. PsWater/debug27 retains the accepted
// projected comparison; normal PsWaterWorld uses native geometry/materials on every retained ray segment.
#include "mml/gfx/water_optics.hlsli"
#include "mml/gfx/environment_emitter.hlsli"
#ifndef MML_WATER_PATH_LOOP
#define MML_WATER_PATH_LOOP [loop]
#endif
#ifndef MML_WATER_SKY_LOOP
#define MML_WATER_SKY_LOOP [unroll]
#endif
float3 IncidentSky(float3 direction,float lod) {
    return g_environmentInfo.y>.5 ? g_environment.SampleLevel(g_clamp,direction,lod).rgb : Lin(WaterSky(direction));
}
float3 CloudOpening(float3 direction,float lod){
    if(g_environmentInfo.w<.5)return 0;
    return EnvEmitterRadiance(IncidentSky(direction,lod),direction,g_emitter[0],g_emitter[1],g_emitter[2],kWaterPi*WaterSunWhite());
}
static float g_traceLevel,g_traceStrength;
#if defined(MML_WATER_SEED_ORDERED)
#include "river_seed_bounds.hlsli"
#endif
float3 WaterTraceDelta(float2 base) {
    float3 delta=0;
    [unroll]for(uint c=0;c<3;c++)delta+=g_displacement.SampleLevel(g_wrap,float3(base*g_cascade[c],c),0).xyz;
    float strength=g_traceStrength;
#if defined(MML_WATER_WORLD_MATERIAL)
#if defined(MML_WATER_SEED_ORDERED)
    [branch]if(g_seedBoundsEnabled)strength*=RiverTraceShore(float3(base.x,g_traceLevel,base.y));
    else
#endif
    strength*=WorldShoreWeight(float3(base.x,g_traceLevel,base.y));
#endif
    return delta*strength;
}
float2 WaterTraceBase(float2 world) {
    float2 base=world;
#if defined(MML_WATER_WORLD_MATERIAL)
    MML_WATER_PATH_LOOP for(uint iteration=0;iteration<3;iteration++) {
#else
    [unroll]for(uint iteration=0;iteration<3;iteration++) {
#endif
        float3 delta=WaterTraceDelta(base);
        base=world+delta.yz*g_light.w;
    }
    return base;
}
float WaterTraceHeight(float2 world) {return g_traceLevel+WaterTraceDelta(WaterTraceBase(world)).x;}
#include "mml/gfx/water_trace.hlsli"
float3 WaterTraceNormal(float3 p) {
    float2 base=WaterTraceBase(p.xz),sl=0;float4 sh=0;
    [unroll]for(uint c=0;c<3;c++) {
        float3 at=float3(base*g_cascade[c],c);
        sl+=g_slopes.SampleLevel(g_wrap,at,0).xy;sh+=g_shape.SampleLevel(g_wrap,at,0);
    }
    float strength=g_traceStrength;
#if defined(MML_WATER_WORLD_MATERIAL)
    strength*=WorldShoreWeight(float3(base.x,g_traceLevel,base.y));
#endif
    return WaterSurfaceSamples(sl,0,sh,0,strength,g_light.w).normal;
}
float2 WaterSurfaceMotion(VOut i,float3 p,bool repaired) {
    float4 previous=i.previous;
    if(repaired && g_temporal.z>.5) {
        float2 base=WaterTraceBase(p.xz);float3 before=0;
        [unroll]for(uint c=0;c<3;c++)before+=g_previousDisplacement.SampleLevel(g_wrap,float3(base*g_cascade[c],c),0).xyz;
        float3 q=float3(base.x,i.base.y,base.y)+float3(-before.y*g_light.w,before.x,-before.z*g_light.w)*g_traceStrength;
        previous=mul(float4(q,1),g_previousVP);
    }
    if(g_temporal.z>.5 && previous.w>1e-5) {
        float2 previousPixel=(previous.xy/previous.w*float2(.5,-.5)+.5)*g_size.xy;
        return previousPixel-(i.pos.xy-g_temporal.xy);
    }
    return 0;
}
// Every segment competes with the next water facet; a nearer hull wins BEFORE the water bounce.
#if defined(MML_WATER_WORLD_TRANSPORT)
#include "river_path_footprint.hlsli"
#if defined(MML_WATER_STAGED_UP)
// Only opaque hits need a payload. The lower two kind bits distinguish
// an escape, an opaque hit, unknown coverage and an ordered continuation.
static const uint WaterEscape=0,WaterOpaque=1,WaterUnknown=2,WaterLayered=3;
struct WaterFirstOpaque {
    float2 bary;
    float distance;
    uint triangleIndex;
    uint material;
};
RWStructuredBuffer<uint> g_waterFirstKinds:register(u4);
RWStructuredBuffer<WaterFirstOpaque> g_waterFirstOpaque:register(u5);
#endif
#if defined(MML_WATER_FAST_UP)
static uint g_waterFirstKind,g_waterFirstPixel;
#endif
// A finite TOTAL camera -> water -> object length bounds native residency independently of view/FOV.
// Environment radiance remains directional at infinity. Missing supported geometry is explicit magenta.
float3 WaterWorldPath(float3 origin,float3 direction,float level,float cameraDistance,float skyLod,
                      float shadow,bool waveTransport,MmlRayFootprint footprint,out float3 openingOut,out float firstDistance){
#if defined(MML_WATER_FAST_UP)
    openingOut=0;
    firstDistance=g_atmosphereInfo.y;
    const uint kind=g_waterFirstKind&3;
    if(kind==WaterUnknown){
        g_worldMaterialUnknown=true;
        return float3(1,0,1);
    }
    if(kind==WaterOpaque){
        const WaterFirstOpaque hit=g_waterFirstOpaque[g_waterFirstPixel];
        g_worldFootprint=footprint;
        g_worldRayOrigin=origin;
        g_worldRayDirection=direction;
        g_worldSelectedInstance=g_waterFirstKind>>2;
        WaterWorldHitContext(hit.triangleIndex,hit.distance);
        float4 encoded;
        uint material;
        if(!MmlSurfaceEncodedAt(hit.triangleIndex,hit.bary,0,hit.material,encoded,material)){
            g_worldMaterialUnknown=true;
            return float3(1,0,1);
        }
        firstDistance=hit.distance;
        return MmlNativeLinear(encoded.rgb);
    }
    openingOut=CloudOpening(direction,skyLod)*shadow*max(g_light.x,0);
    const float3 sky=IncidentSky(direction,skyLod)+openingOut;
    return ApplyAtmosphereAfterTravel(sky,origin,direction,g_atmosphereInfo.y,cameraDistance,true);
#else
    float3 radiance=0;float throughput=1,travel=0;openingOut=0;firstDistance=g_atmosphereInfo.y;
    MML_WATER_PATH_LOOP for(uint segment=0;segment<3;segment++){
        WaterWaveHit wave=(WaterWaveHit)0;
        bool waterLimited=direction.y<=0&&waveTransport;
        if(waterLimited&&segment>=2)return radiance; // retained two-water-bounce transport bound
        if(waterLimited){
            origin+=float3(0,.015,0);travel+=.015;
            wave=WaterTraceWaves(origin,direction,level,max(g_geometry.z,.05),40);
        }
        float remaining=max(g_worldPath.x-cameraDistance-travel,0);
        // An unresolved water search certifies only its traversed prefix, not the rest of the object range.
        float limit=waterLimited?min(remaining,wave.distance):remaining;
        MmlSceneRadiance scene=(MmlSceneRadiance)0;scene.transmission=1;
        g_worldFootprint=footprint;g_worldRayOrigin=origin;g_worldRayDirection=direction;
        if(limit>.02)scene=MmlTraceSceneRadiance(origin,direction,.02,limit,0);
        if(scene.status>1){g_worldMaterialUnknown=true;return float3(1,0,1);}
        radiance+=throughput*scene.color;
        if(segment==0&&scene.layers>0)firstDistance=scene.first.distance;
        throughput*=scene.transmission;
        if(throughput==0)return radiance;
        if(wave.status!=1){
            if(direction.y>0){
                float3 opening=CloudOpening(direction,skyLod)*shadow*max(g_light.x,0);
                float3 sky=IncidentSky(direction,skyLod)+opening;
                radiance+=throughput*ApplyAtmosphereAfterTravel(sky,origin,direction,g_atmosphereInfo.y,cameraDistance+travel,true);
                openingOut+=throughput*opening;
            }
            return radiance;
        }
        float3 normal,normalX,normalZ;WaterBounceNormals(wave,normal,normalX,normalZ);
        float3 outgoing=-direction;
        float nextF=WaterFresnel(max(dot(outgoing,normal),0));float3 body=0;
        MML_WATER_SKY_LOOP for(uint k=0;k<4;k++){
            float mu=(k+.5)/4,phi=2*kWaterPi*WaterOpticalSample(k).y,radius=sqrt(1-mu*mu);
            float3 light=float3(radius*cos(phi),mu,radius*sin(phi));
            body+=IncidentSky(light,3)*(2*kWaterPi/4)*WaterMediumBeam(outgoing,normal,light,
                g_absorb.a,g_absorb.rgb,max(g_optics.x,0)*max(g_scatter.rgb,0),g_optics.y);
        }
        radiance+=throughput*(1-nextF)*body;throughput*=nextF;
        WaterBounceFootprint(footprint,origin,direction,wave,normal,normalX,normalZ);
        travel+=wave.distance;origin=wave.p;direction=reflect(direction,normal);
    }
    return radiance;
#endif
}
#endif
#if defined(MML_WATER_WORLD_TRANSPORT)
struct WaterRaySeed {
    float4 a:SV_Target0;
    float4 b:SV_Target1;
    float4 c:SV_Target2;
    float4 d:SV_Target3;
    float4 e:SV_Target4;
    float4 f:SV_Target5;
    float4 g:SV_Target6;
    float4 h:SV_Target7;
    float depth:SV_Depth;
};
Texture2DArray<float4> g_waterSeeds:register(t33);
Texture2DArray<float4> g_waterDerivatives:register(t34);
Texture2DArray<float4> g_waterAdjacent:register(t35);
#if defined(MML_WATER_SEED_ORDERED)
RasterizerOrderedTexture2DArray<float4> g_orderedSeeds:register(u0);
RasterizerOrderedTexture2DArray<float4> g_orderedDerivatives:register(u1);
RasterizerOrderedTexture2DArray<float4> g_orderedAdjacent:register(u2);
RasterizerOrderedTexture2D<float> g_orderedDepth:register(u3);
RasterizerOrderedTexture2D<float4> g_orderedSurface:register(u4);
#else
RWStructuredBuffer<float4> g_waterTransportOut:register(u0);
#endif
#endif
#if defined(MML_WATER_WORLD_MATERIAL)
cbuffer WaterTransportSample:register(b5){uint2 g_transportSize;uint g_transportSample;uint g_transportResolve;};
#endif
#if defined(MML_WATER_WORLD_TRANSPORT)
#if defined(MML_WATER_SEED_ORDERED)
void PsWaterSeedOrdered(VOut i) {
#else
WaterRaySeed PsWaterSeed(VOut i) {
#endif
#else
#if defined(MML_WATER_WORLD_MATERIAL)
StructuredBuffer<float4> g_waterTransport:register(t33);
Texture2DArray<float4> g_resolveSeeds:register(t34);
Texture2DArray<float4> g_resolveDerivatives:register(t35);
Texture2D<float4> g_resolveSurface:register(t36);
Texture2D<float> g_resolveDepth:register(t37);
#endif
WaterPixel WaterShade(VOut i,bool resolve) {
#endif
    float2 px=i.pos.xy;
    float3 p,v,meanRay,rayDx,rayDy;
    float z,dist;
    float2 uvFlat,uvDx,uvDy;
    WaterSurface s;
    WaterOpticalFrame optical;
    bool repaired=false;
#if defined(MML_WATER_SEED_ORDERED)
    g_seedBoundsEnabled=(g_transportResolve&2u)!=0;
    g_emptyShoreCell=0;
#endif
#if defined(MML_WATER_WORLD_MATERIAL)
    const bool waveTransport=true; // old single-scattering diagnostic21 is compiled/routed separately
#else
    bool waveTransport=(int)g_light.z!=21; // developer A/B view: the prior single-scattering implementation
#endif
#if defined(MML_WATER_WORLD_MATERIAL) && !defined(MML_WATER_WORLD_TRANSPORT)
    uint input=(uint(px.y)*g_transportSize.x+uint(px.x))*3;
    float4 facets=g_waterTransport[input+2];
    float4 surface;
    [branch]if(resolve){
        if(facets.w==0)discard;
        uint2 pixel=uint2(px);
        surface=g_resolveSurface.Load(int3(pixel,0));
        p=g_resolveSeeds.Load(int4(pixel,0,0)).xyz;
        v=g_resolveSeeds.Load(int4(pixel,1,0)).xyz;
        float4 n=g_resolveSeeds.Load(int4(pixel,2,0)),t=g_resolveSeeds.Load(int4(pixel,3,0));
        optical.n=n.xyz;optical.t=t.xyz;optical.b=g_resolveDerivatives.Load(int4(pixel,0,0)).xyz;
        optical.alpha=float2(n.w,t.w);
        s=(WaterSurface)0;s.normal=n.xyz;s.jacobian=surface.w;
        z=surface.x;
    }else
#endif
    {
    p=i.world;
    float3 toEye=g_eye.xyz-p;
    z=dot(-toEye,g_fwd.xyz);dist=max(length(toEye),1e-5);
    v=toEye/dist;
#if defined(MML_WATER_SEED_ORDERED)
    [branch]if(g_seedBoundsEnabled&&RiverSeedQuadOccluded(-v,z,dist,px,i.base.y))discard;
#endif
    uvFlat=WaterReflectionUv(p,g_rvp);uvDx=ddx(uvFlat);uvDy=ddy(uvFlat);
    s=WaterRipples(g_slopes,g_shape,g_wrap,i.base.xz,g_cascade.xyz,g_cascade.w,
                               g_water.y*g_geometry.x*i.shore,g_light.w);
    g_traceLevel=i.base.y;g_traceStrength=g_water.y*g_geometry.x*i.shore;
#if defined(MML_WATER_WORLD_MATERIAL)
    g_traceStrength=g_water.y*g_geometry.x; // continuation samples contact locally, not at its first facet
#endif
    // Fine normal-map waves can hide a coarse raster patch. Find the actual front-facing water intersection
    // instead of shading an invisible back face black. The mesh still supplies the conservative coverage.
    if(waveTransport && dot(s.normal,v)<=.025 && g_geometry.y>.5) {
#if defined(MML_WATER_SEED_ORDERED)
        [branch]if(!g_seedBoundsEnabled||!RiverNoWaveCrossing(-v,dist+2,true))
#endif
        {
            WaterWaveHit visible=WaterTraceWaves(g_eye.xyz,-v,i.base.y,max(g_geometry.z,.05),dist+2);
            if(visible.status==1){p=visible.p;s.normal=WaterTraceNormal(p);toEye=g_eye.xyz-p;dist=length(toEye);z=dot(-toEye,g_fwd.xyz);repaired=true;}
        }
    }
    float3 normalDx=ddx(s.normal),normalDy=ddy(s.normal);
    meanRay=reflect(-v,s.normal);
    rayDx=ddx(meanRay);rayDy=ddy(meanRay);
    optical=WaterOpticalBasis(s.normal,s.var*g_shadow.z*g_shadow.z,normalDx,normalDy);
#if !defined(MML_WATER_WORLD_TRANSPORT)
    if(g_eye.y<=i.base.y || z>SceneDepth(px)+.02) discard;
    }
#endif
#if defined(MML_WATER_WORLD_TRANSPORT)
    float3 positionDx=ddx(p),positionDy=ddy(p),viewDx=ddx(v),viewDy=ddy(v);
    WaterOpticalFrame opticalX=WaterAdjacentOptical(optical,normalDx,ddx(optical.t),ddx(optical.alpha));
    WaterOpticalFrame opticalY=WaterAdjacentOptical(optical,normalDy,ddy(optical.t),ddy(optical.alpha));
    if(g_eye.y<=i.base.y || z>SceneDepth(px)+.02) discard;
    WaterRaySeed seed=(WaterRaySeed)0;
    float4 clip=mul(float4(p,1),g_vp);
    seed.depth=clip.z/clip.w;
    uint count=max(optical.alpha.x,optical.alpha.y)>.0001?4u:1u;
#if !defined(MML_WATER_SEED_ORDERED)
    // Both raster passes use the same depth and discard. Store the original
    // quad's full-precision frames so compute can evaluate all four facets.
    if(g_transportSample!=0){
        seed.a=float4(opticalX.n,opticalX.alpha.x);
        seed.b=float4(opticalX.t,opticalX.alpha.y);
        seed.c=float4(opticalY.n,opticalY.alpha.x);
        seed.d=float4(opticalY.t,opticalY.alpha.y);
        return seed;
    }
#endif
    float3 vx=normalize(v+viewDx),vy=normalize(v+viewDy);
    float lod=log2(max(.5*g_environmentInfo.x*max(length(rayDx),length(rayDy)),1));
    bool unknown=i.shoreUnknown>.0001||g_worldShoreUnknown;
    seed.a=float4(p,i.base.y);
    seed.b=float4(v,lod);
    seed.c=float4(optical.n,optical.alpha.x);
    seed.d=float4(optical.t,optical.alpha.y);
    seed.e=float4(optical.b,unknown?-float(count):float(count));
    seed.f=float4(positionDx,vx.x);
    seed.g=float4(positionDy,vx.y);
    seed.h=float4(vy,vx.z);
#if defined(MML_WATER_SEED_ORDERED)
    uint2 pixel=uint2(px);
    float4 surface=0;
    [branch]if((g_transportResolve&1u)!=0)surface=float4(z,WaterSurfaceMotion(i,p,repaired),s.jacobian);
    float depth=saturate(seed.depth);
    // Match D32_FLOAT LESS_EQUAL, including the later submitted fragment winning a tie.
    // Discard masks the stores while retaining helper lanes for quad derivatives.
    if(depth>g_orderedDepth[pixel])discard;
    g_orderedDepth[pixel]=depth;
    g_orderedSeeds[uint3(pixel,0)]=seed.a;
    g_orderedSeeds[uint3(pixel,1)]=seed.b;
    g_orderedSeeds[uint3(pixel,2)]=seed.c;
    g_orderedSeeds[uint3(pixel,3)]=seed.d;
    g_orderedDerivatives[uint3(pixel,0)]=seed.e;
    g_orderedDerivatives[uint3(pixel,1)]=seed.f;
    g_orderedDerivatives[uint3(pixel,2)]=seed.g;
    g_orderedDerivatives[uint3(pixel,3)]=seed.h;
    g_orderedAdjacent[uint3(pixel,0)]=float4(opticalX.n,opticalX.alpha.x);
    g_orderedAdjacent[uint3(pixel,1)]=float4(opticalX.t,opticalX.alpha.y);
    g_orderedAdjacent[uint3(pixel,2)]=float4(opticalY.n,opticalY.alpha.x);
    g_orderedAdjacent[uint3(pixel,3)]=float4(opticalY.t,opticalY.alpha.y);
    [branch]if((g_transportResolve&1u)!=0)g_orderedSurface[pixel]=surface;
#else
    return seed;
#endif
    }
}

#if !defined(MML_WATER_SEED_ORDERED)
WaterOpticalFrame WaterSeedAdjacent(uint2 pixel,uint slice) {
    float4 n=g_waterAdjacent.Load(int4(pixel,slice,0)),t=g_waterAdjacent.Load(int4(pixel,slice+1,0));
    WaterOpticalFrame optical;
    optical.n=n.xyz;optical.t=t.xyz;optical.b=cross(n.xyz,t.xyz);optical.alpha=float2(n.w,t.w);
    return optical;
}

#if defined(MML_WATER_COMPACT_QUEUE)
#include "river_ray_queue.hlsli"
#if defined(MML_WATER_STAGED_UP)
[numthreads(64,1,1)]
void CsWaterFirstHit(uint3 task:SV_DispatchThreadID) {
    uint3 id;
    if(!WaterQueuePixel(task.x,id))return;
    const uint pixel=id.y*g_transportSize.x+id.x;
    const float4 centerB=g_waterDerivatives.Load(int4(id.xy,0,0));
    const uint count=(uint)abs(centerB.w);
    const float3 view=g_waterSeeds.Load(int4(id.xy,1,0)).xyz;
    const float4 centerN=g_waterSeeds.Load(int4(id.xy,2,0));
    const float4 centerT=g_waterSeeds.Load(int4(id.xy,3,0));
    WaterOpticalFrame optical;
    optical.n=centerN.xyz;
    optical.t=centerT.xyz;
    optical.b=centerB.xyz;
    optical.alpha=float2(centerN.w,centerT.w);
    const float3 facet=count>1?WaterVisibleFacet(optical,view,WaterOpticalSample(g_transportSample,count)):optical.n;
    const float3 direction=reflect(-view,facet);
    uint kind=WaterEscape,selected=0;
    // A grazing ray or an alpha layer stays in the original ordered tracer.
    if(direction.y<=0)kind=WaterLayered;
    else if(WaterReflectionWeight(optical,view,direction,facet)/count>1e-8){
        const float3 origin=g_waterSeeds.Load(int4(id.xy,0,0)).xyz;
        const float cameraDistance=max(length(g_eye.xyz-origin),1e-5);
        const float remaining=max(g_worldPath.x-cameraDistance,0);
        if(remaining>.02){
            g_worldFootprint.observer=WaterMirrorObserver(g_eye.xyz,origin,facet);
            mml_orderCursor=false;
            const MmlRayHit hit=MmlTraceWorld(origin,direction,.02,remaining);
            if(hit.status>1)kind=WaterUnknown;
            else if(hit.status==1){
                uint materials,stride;
                mml_materials.GetDimensions(materials,stride);
                kind=WaterLayered;
                if(mml_rayCoverageLo.w!=0&&hit.material<materials&&g_worldSelectedInstance<0x40000000u){
                    const MmlSceneMaterial material=mml_materials[hit.material];
                    if(material.info.x==0&&material.blend.x==0){
                        WaterFirstOpaque opaque;
                        opaque.bary=hit.bary;
                        opaque.distance=hit.distance;
                        opaque.triangleIndex=hit.triangleIndex;
                        opaque.material=hit.material;
                        g_waterFirstOpaque[pixel]=opaque;
                        kind=WaterOpaque;
                        selected=g_worldSelectedInstance;
                    }
                }
            }
        }
    }
    g_waterFirstKinds[pixel]=(selected<<2)|kind;
    WaterQueueAppend(2,kind==WaterLayered,pixel);
}
#endif
[numthreads(64,1,1)]
void CsWaterTransport(uint3 task:SV_DispatchThreadID) {
    uint3 id;
    if(!WaterQueuePixel(task.x,id))return;
#else
[numthreads(8,8,1)]
void CsWaterTransport(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=g_transportSize))return;
#endif
    uint output=(id.y*g_transportSize.x+id.x)*3;
#if defined(MML_WATER_FAST_UP)
    g_waterFirstPixel=output/3;
    g_waterFirstKind=g_waterFirstKinds[g_waterFirstPixel];
    if((g_waterFirstKind&3)==WaterLayered)return;
#endif
    float4 centerB=g_waterDerivatives.Load(int4(id.xy,0,0));
    uint count=(uint)abs(centerB.w);
    if(g_transportSample>=count){
        if(g_transportSample==0){
            g_waterTransportOut[output]=0;
            g_waterTransportOut[output+1]=0;
            g_waterTransportOut[output+2]=0;
        }
        return;
    }
    float4 reflection=0,opening=0,facets=0;
    if(g_transportSample!=0){
        reflection=g_waterTransportOut[output];
        opening=g_waterTransportOut[output+1];
        facets=g_waterTransportOut[output+2];
    }
    float4 view=g_waterSeeds.Load(int4(id.xy,1,0));
    float4 centerN=g_waterSeeds.Load(int4(id.xy,2,0)),centerT=g_waterSeeds.Load(int4(id.xy,3,0));
    WaterOpticalFrame optical;
    optical.n=centerN.xyz;optical.t=centerT.xyz;optical.b=centerB.xyz;optical.alpha=float2(centerN.w,centerT.w);
    float2 sample=WaterOpticalSample(g_transportSample,count);
    float3 facet=count>1?WaterVisibleFacet(optical,view.xyz,sample):optical.n;
    float3 ray=reflect(-view.xyz,facet);
    float weight=WaterReflectionWeight(optical,view.xyz,ray,facet)/count;
    float fresnel=WaterFresnel(max(dot(view.xyz,facet),0));
    reflection.a+=(1-fresnel)/count;
    facets.x+=fresnel/count;
    if(ray.y<=0)facets.y+=1/count;
    if(weight<=1e-8)facets.z+=1/count;
    else{
        float4 surface=g_waterSeeds.Load(int4(id.xy,0,0));
        float4 originDx=g_waterDerivatives.Load(int4(id.xy,1,0));
        float4 originDy=g_waterDerivatives.Load(int4(id.xy,2,0));
        float4 views=g_waterDerivatives.Load(int4(id.xy,3,0));
        float3 vx=float3(originDx.w,originDy.w,views.w),vy=views.xyz;
        WaterOpticalFrame opticalX=WaterSeedAdjacent(id.xy,0),opticalY=WaterSeedAdjacent(id.xy,2);
        float3 facetX=count>1?WaterVisibleFacet(opticalX,vx,sample):opticalX.n;
        float3 facetY=count>1?WaterVisibleFacet(opticalY,vy,sample):opticalY.n;
        MmlRayFootprint footprint;
        footprint.originDx=originDx.xyz;footprint.originDy=originDy.xyz;
        footprint.directionDx=reflect(-vx,facetX)-ray;footprint.directionDy=reflect(-vy,facetY)-ray;
        footprint.observer=WaterMirrorObserver(g_eye.xyz,surface.xyz,facet);
        footprint.observerDx=WaterMirrorObserver(g_eye.xyz,surface.xyz+originDx.xyz,facetX)-footprint.observer;
        footprint.observerDy=WaterMirrorObserver(g_eye.xyz,surface.xyz+originDy.xyz,facetY)-footprint.observer;
        g_traceLevel=surface.w;g_traceStrength=g_water.y*g_geometry.x;
        float3 cloudOpening;float distance;
        reflection.rgb+=weight*WaterWorldPath(surface.xyz,ray.xyz,surface.w,
            max(length(g_eye.xyz-surface.xyz),1e-5),view.w,SunShadowSoft(surface.xyz,g_shadow.x),true,
            footprint,cloudOpening,distance);
        opening.rgb+=weight*cloudOpening;
        if(g_transportSample==0)opening.a=distance;
    }
    facets.w=max(facets.w,centerB.w<0||g_worldShoreUnknown||g_worldMaterialUnknown?2:1);
    g_waterTransportOut[output]=reflection;
    g_waterTransportOut[output+1]=opening;
    g_waterTransportOut[output+2]=facets;
}
#endif
#else
    float shadow=SunShadowSoft(p,g_shadow.x);
    float3 sun=g_environmentInfo.z>.5 ? g_sun.xyz : WaterSunSeen();
    bool haveSun=g_environmentInfo.z>.5 ? g_sun.w>.5 : (g_sunSeen.w>.5 || g_sun.w>.5);
    float3 solarIrradiance=haveSun ? kWaterPi*WaterSunWhite() : 0;
    float3 reflected=0,openingReflection=0;
    float fresnel=0,transmission=0,bankShare=0,hitT=0,belowHorizon=0,masked=0;
#if !defined(MML_WATER_WORLD_MATERIAL)
    float lod=log2(max(.5*g_environmentInfo.x*max(length(rayDx),length(rayDy)),1));
    float2 up=uvDy/max(length(uvDy),1e-9)*g_reflSize.w;
    float planarLod=log2(max(max(length(uvDx*g_reflSize.xy),length(uvDy*g_reflSize.xy)),1));
    float pxPerRadian=g_reflSize.y*g_proj.y*.5;
    bool planar=g_water.z>.5 && abs(i.base.y-g_water.x)<.05 && all(uvFlat>=0) && all(uvFlat<=1);
    uint count=max(optical.alpha.x,optical.alpha.y)>.0001 ? (waveTransport ? 4u : 8u) : 1u;
    MML_WATER_PATH_LOOP for(uint j=0;j<count;j++) {
        float2 sample=WaterOpticalSample(j,count);
        float3 facet=count>1 ? WaterVisibleFacet(optical,v,sample) : s.normal;
        float f=WaterFresnel(max(dot(v,facet),0));
        fresnel+=f/count;transmission+=(1-f)/count;
        float3 ray=reflect(-v,facet);
        float weight=WaterReflectionWeight(optical,v,ray,facet)/count;
        if(ray.y<=0) belowHorizon+=1.0/count;
        if(weight<=1e-8) masked+=1.0/count;
        if(weight<=1e-8) continue;
        float3 rayOrigin=p;
        float waveTravel=0;
        float3 waveBody=0;
        // A downward air ray meets another water facet. Continue through that real world-space hit instead
        // of discarding its complete radiance or bending it into the sky. Bounded two-bounce approximation.
        [loop]for(uint bounce=0;bounce<2 && ray.y<=0 && waveTransport;bounce++) {
            WaterWaveHit wave=WaterTraceWaves(rayOrigin+float3(0,.015,0),ray,i.base.y,max(g_geometry.z,.05),40);
            if(wave.status!=1)break;
            float3 nextNormal=WaterTraceNormal(wave.p);
            float3 outgoing=-ray;
            float nextF=WaterFresnel(max(dot(outgoing,nextNormal),0));
            float3 nextBody=0;
            [unroll]for(uint k=0;k<4;k++) {
                float mu=(k+.5)/4,phi=2*kWaterPi*WaterOpticalSample(k).y;
                float radius=sqrt(1-mu*mu);
                float3 light=float3(radius*cos(phi),mu,radius*sin(phi));
                nextBody+=IncidentSky(light,3)*(2*kWaterPi/4)*WaterMediumBeam(outgoing,nextNormal,light,
                    g_absorb.a,g_absorb.rgb,max(g_optics.x,0)*max(g_scatter.rgb,0),g_optics.y);
            }
            waveBody+=weight*(1-nextF)*nextBody;
            weight*=nextF;waveTravel+=wave.distance;rayOrigin=wave.p;
            ray=reflect(ray,nextNormal);
        }
        reflected+=waveBody;
        if(ray.y<=0)continue;
        float3 opening=CloudOpening(ray,lod)*shadow*max(g_light.x,0);
        float3 radiance=IncidentSky(ray,lod)+opening;
        float distance=g_atmosphereInfo.y;bool sky=true;
        // Temporary object-only path. Background ALWAYS comes from the directional cube; a screen border or
        // pixel of the planar sky is never used as illumination. This still lacks hidden-object geometry.
        if(planar && dot(ray,g_rdepth.xyz)>1e-4) {
            float2 originUv=WaterReflectionUv(rayOrigin,g_rvp);
            WaterHit hit=WaterFollowReflection(rayOrigin,ray,originUv,g_rvp,g_rdepth,g_reflDepth,up,g_rinv,pxPerRadian);
            if(!hit.sky && all(hit.uv>=0) && all(hit.uv<=1)) {
                radiance=g_refl.SampleLevel(g_clamp,hit.uv,hit.contact?0:planarLod).rgb;
                if(g_light.y>.5) radiance=Lin(radiance);
                distance=hit.t;sky=false;bankShare+=weight;
                opening=0;
            }
        }
        reflected+=weight*ApplyAtmosphereAfterTravel(radiance,rayOrigin,ray,distance,dist+waveTravel,sky);
        openingReflection+=weight*opening;
        if(j==0) hitT=distance;
    }
#else
    if(facets.w==0)discard;
    float4 reflection=g_waterTransport[input];
    float4 opening=g_waterTransport[input+1];
    reflected=reflection.rgb;transmission=reflection.a;
    openingReflection=opening.rgb;hitT=opening.a;
    fresnel=facets.x;belowHorizon=facets.y;masked=facets.z;
#endif
    // Scattering color now specifies a coefficient, not an independent colored emitter. The retained scalar
    // Turbidity is its density, per metre. Absorption keeps its existing per-metre color/clarity conversion.
    float3 sigmaS=max(g_optics.x,0)*max(g_scatter.rgb,0);
    float3 body=0,skyIrradiance=0;
    MML_WATER_SKY_LOOP for(uint skyIndex=0;skyIndex<8;skyIndex++) {
        float mu=(skyIndex+.5)/8,phi=2*kWaterPi*WaterOpticalSample(skyIndex).y;
        float radial=sqrt(max(1-mu*mu,0));
        float3 light=float3(radial*cos(phi),mu,radial*sin(phi));
        float3 beam=IncidentSky(light,3)*(2*kWaterPi/8);
        body+=beam*WaterMediumBeam(v,s.normal,light,g_absorb.a,g_absorb.rgb,sigmaS,g_optics.y);
        skyIrradiance+=beam*mu;
    }
    float3 sunIn=0,surfaceEmitterIrradiance=0;
    if(g_environmentInfo.w>.5 && g_emitter[0].w>.5){
        [loop]for(uint packet=0;packet<16;packet++){
            float3 direction=g_emitter[3+packet*2].xyz;
            float3 beam=g_emitter[4+packet*2].rgb*solarIrradiance/max(g_emitter[1].rgb,1e-8);
            sunIn+=beam*shadow*WaterMediumBeam(v,s.normal,direction,g_absorb.a,g_absorb.rgb,sigmaS,g_optics.y);
            surfaceEmitterIrradiance+=beam*max(direction.y,0);
        }
    }else if(g_environmentInfo.z>.5){
        sunIn=solarIrradiance*shadow*WaterMediumBeam(v,s.normal,sun,g_absorb.a,g_absorb.rgb,sigmaS,g_optics.y);
        surfaceEmitterIrradiance=solarIrradiance*max(sun.y,0);
    }
    body+=sunIn;
    // Native painted-sky mode uses the measured cloud opening above, not an assumed naked solar disc.
    float3 solarReflection=g_environmentInfo.z>.5 ? solarIrradiance*shadow*max(g_light.x,0)*WaterDirectSun(optical,v,sun) : 0;
    float foam=saturate(g_scatter.a*WaterWhitecaps(s.jacobian,p.xz,g_water.w));
    float3 foamLight=.8*(skyIrradiance+surfaceEmitterIrradiance*shadow)/kWaterPi;
    float3 col=(reflected*g_shadow.w+solarReflection*g_shadow.w+body*transmission)*(1-foam)+foamLight*foam;
    // Shared atmosphere will integrate the incoming camera leg after this replacement's displaced depth.
    // Native fog is retained as a fallback only while the atmosphere service is off.
    if(g_atmosphereInfo.x<.5) col=lerp(col,Lin(g_fogColor.rgb),FogAmount(z));
    int debug=resolve?0:(int)g_light.z;
    if(debug==21)debug=0;
    if(debug==1) col=reflected;
    else if(debug==2) col=Lin(s.normal*.5+.5);
    else if(debug==3) col=body*transmission;
    else if(debug==4) col=fresnel;
    else if(debug==5) col=Lin(saturate(sqrt(s.var.xyy)*4));
    else if(debug==6) col=IncidentSky(meanRay,0);
    else if(debug==7) col=sunIn*transmission;
    else if(debug==8) col=shadow;
    else if(debug==9) col=Lin(saturate(float3(1-s.jacobian,s.jacobian,0)));
#if !defined(MML_WATER_WORLD_MATERIAL)
    else if(debug==10) { col=g_refl.SampleLevel(g_clamp,uvFlat,0).rgb;if(g_light.y>.5) col=Lin(col); }
#endif
    else if(debug==11) col=saturate(float3(hitT/20,hitT/100,hitT>=2999 ? 1 : 0));
    else if(debug==13) col=Lin(saturate(float3(abs(s.slope)*10,sqrt(s.var.x)*10)));
    else if(debug==15) col=float3(saturate(bankShare),0,1-saturate(bankShare));
#if !defined(MML_WATER_WORLD_MATERIAL)
    else if(debug==16) col=g_skyReflection.SampleLevel(g_clamp,uvFlat,0).rgb;
#endif
    else if(debug==17) col=solarReflection+openingReflection;
    else if(debug==18) col=skyIrradiance/kWaterPi;
    else if(debug==19) col=float3(belowHorizon,masked,saturate(dot(optical.n,v)));
    else if(debug==20) col=i.shore;
    WaterPixel outPixel;
    outPixel.color=float4(Enc(debug==0 ? WaterDisplayRadiance(col) : col),1);
#if defined(MML_WATER_WORLD_MATERIAL)
    if(facets.w==2||(!resolve&&(i.shoreUnknown>.0001||g_worldShoreUnknown)))outPixel.color=float4(1,0,1,1);
    if(resolve){
        outPixel.depth=g_resolveDepth.Load(int3(uint2(px),0));
        outPixel.temporal=float4(surface.xyz,1);
        return outPixel;
    }
#endif
    float4 clip=mul(float4(p,1),g_vp);
    outPixel.depth=clip.z/clip.w;
    outPixel.temporal=float4(z,WaterSurfaceMotion(i,p,repaired),1);
    return outPixel;
}
#if defined(MML_WATER_WORLD_MATERIAL)
WaterPixel PsWaterWorld(VOut i){return WaterShade(i,false);}
WaterPixel PsWaterResolve(float4 position:SV_Position){
    VOut i=(VOut)0;i.pos=position;return WaterShade(i,true);
}
#else
WaterPixel PsWater(VOut i){return WaterShade(i,false);}
#endif
#endif
