// river_water's water (river_water.cpp): the game's water drawn anew on its own triangles, captured from LS3D, by the
// SDK's water pieces (mml/gfx/water.hlsli).
//
// Lighting preview: river_lighting.hlsli combines a world-directional native sky cube, visible Beckmann facets,
// a separate finite-disc solar BRDF and homogeneous single-scattering medium. The wave field and displaced
// temporal depth/motion stay unchanged. Object reflections still use the legacy planar tracer until the shared
// world geometry/material query service is implemented; moving-occluder coverage is NOT solved by this preview.
// Historical painted-sky kernels below remain explicit diagnostics/tests, not the normal illumination source.
#include "mml/gfx/frame.hlsli"
#include "mml/gfx/water.hlsli"

Texture2D<float4> g_refl : register(t4);         // the reflection, its mips (sRGB: read in linear terms)
Texture2D<float> g_reflDepth : register(t5);     // its linear depth
Texture2DArray<float4> g_slopes : register(t6);  // each cascade: slope x, slope z, their squares (mipmapped)
Texture2DArray<float4> g_shape : register(t7);   // each cascade: the height, dDx/dx, dDz/dz, dDx/dz (mipmapped)
Texture2DArray<float4> g_displacement : register(t8); // height, horizontal Dx, Dz
Texture2DArray<float4> g_previousDisplacement : register(t9);
TextureCube<float4> g_environment : register(t11);
StructuredBuffer<float4> g_emitter : register(t12);
Texture2D<float4> g_skyReflection : register(t10); // reconstructed scene radiance; alpha is extra solar gain
RWTexture2D<float4> g_skyOutput : register(u0);
SamplerState g_wrap : register(s2);              // anisotropic, wrapping (the waves)
SamplerState g_clamp : register(s3);             // anisotropic, clamped (the reflection)

cbuffer Water : register(b1) {
    row_major float4x4 g_vp;    // the frame's view x projection (row vectors)
    row_major float4x4 g_rvp;   // the reflection's
    float4 g_rdepth;            // a world point's linear depth in the reflection: dot(float4(p, 1), g_rdepth)
    float4 g_water;             // x the reflection's level, y the waves' strength, z 1 a reflection this frame, w time
    float4 g_cascade;           // xyz 1 / each cascade's size (m), w the slopes' variance no cascade draws (an axis)
    float4 g_absorb;            // rgb absorption a metre (linear light), a the uniform virtual depth (m)
    float4 g_scatter;           // rgb relative particle scattering coefficient (linear), a foam coverage scale
    float4 g_light;             // x the sun's glitter, y 1 the reflection's copy reads display-encoded, z the debug
                                // view (0 none, 1 the reflection, 2 the normal, 3 the body, 4 Fresnel, 5 roughness, 6 the
                                // reflection unblurred, 7 the sun scattered in the water and through the crests, 8 the
                                // shadows, 9 the Jacobian, 10 the reflection where flat water would show it, 11 how far the reflected ray went, 12 the
                                // reflection's depth and the point's in it (m/100), 13 the slopes), w the waves' chop (their displacement towards the crests)
    float4 g_reflSize;          // the reflection's width, height, 1 / width, 1 / height
    float4 g_optics;            // x the particles' scattering a metre, y its phase's asymmetry g, z the crests' glow,
                                // w the crests' height (2 deviations of the heights, m)
    row_major float4x4 g_rinv;  // the reflection's view x projection inverted
    float4 g_shadow;           // x shadow penumbra, y solar radius cosine, z reflection softness, w reflection strength
    float4 g_geometry;         // x height multiplier, y 3D enabled, z conservative wave bound, w radiance available
    row_major float4x4 g_previousVP; // unjittered camera of the preceding water draw
    float4 g_previousEye;
    float4 g_temporal;         // xy current projection jitter (render pixels), z valid history, w temporal MRT bound
    float4 g_environmentInfo; // x cube face size, y ready, z use legacy scene-light direction
    float4 g_worldPath;       // x finite total camera/water/object path range (world metres)
};

// Expand the actual painted sky's highlights before filtering. The sun frame is only an anchor: placing an
// angular disc there invents a clear sun even when its painting is a bright opening behind a cloud. A bounded
// inverse-tone curve keeps the painting's shape and hue, including cloud coverage. This is an LDR-to-HDR
// reconstruction assumption, not recovered physical radiance; an unclipped HDR sky would not need it.
#if !defined(MML_WATER_WORLD_MATERIAL)
[numthreads(8, 8, 1)]
void CsPaintedSky(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)g_reflSize.xy)) return;
    float3 color = g_refl.Load(int3(id.xy, 0)).rgb;
    if (g_light.y > 0.5) color = Lin(color);
    float solarGain=1;
    if (IsSky(g_reflDepth.Load(int3(id.xy, 0)))) {
        // The game's backdrop is a display-referred painting, not incident sky radiance. Expanding only a cone
        // around the sun left all other bright clouds at LDR intensity: Fresnel could only darken them into grey.
        // A bounded inverse-tone curve preserves the real painting's hue/cloud pattern everywhere.
        // It is an explicit radiometric assumption; clipped source highlights cannot be physically recovered.
        // Neutral brightness distinguishes clouds from a saturated blue sky: max(R,G,B) falsely expanded the
        // blue channel's radiance everywhere and gave the whole river an electric-blue cast.
        float white = min(min(color.r, color.g), color.b);
        color /= max(pow(saturate(1 - white),2.25), 1.0 / 16);
        float2 uv = (float2(id.xy) + 0.5) * g_reflSize.zw;
        float2 ndc = uv * float2(2, -2) + float2(-1, 1);
        float4 a = mul(float4(ndc, 0, 1), g_rinv), b = mul(float4(ndc, 1, 1), g_rinv);
        float3 ray = normalize(b.xyz / b.w - a.xyz / a.w);
        // Broad, smooth support around the actual sun anchor; the painting still supplies every bright shape.
        float cone = saturate((dot(ray, WaterSunSeen()) - g_shadow.y) / max(1 - g_shadow.y, 1e-6));
        float angular = cone * cone * (3 - 2 * cone);
        float bright = saturate((white - 0.53) / 0.47); // roughly display 0.75..1, in linear light
        solarGain = 1 + g_light.x * angular * bright * bright / max(1 - white, 1.0 / 16);
        color *= solarGain;
    } else {
        // Sunlit walls and colored foliage are display-referred too. Leaving them at <=1 while expanding the
        // sky drowned their reflections. A milder bounded expansion (<=2x) retains their actual hue/details.
        float brightness=max(max(color.r,color.g),color.b);
        color/=max(1-.5*brightness,.5);
    }
    g_skyOutput[id.xy] = float4(color, solarGain);
}
#endif

struct VOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 base : TEXCOORD1;    // undisplaced material point: waves and mean-plane reflection share this phase
    float shore : TEXCOORD2;
    float4 previous : TEXCOORD3;
    float shoreUnknown : TEXCOORD4;
};

struct PatchPoint { float3 p : POSITION; };
PatchPoint VsWater(float3 p : POSITION) { PatchPoint o; o.p = p; return o; }

struct PatchFactors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };

float EdgeTess(float3 a, float3 b) {
    float3 ab = b - a;
    float t = saturate(dot(g_eye.xyz - a, ab) / max(dot(ab, ab), 1e-8));
    float dist = length(a + ab * t - g_eye.xyz);
    float spacing = clamp(dist * 0.006, 0.25, 16);
    return clamp(length(ab) / spacing, 1, 64);
}

PatchFactors WaterFactors(InputPatch<PatchPoint, 3> p) {
    PatchFactors f;
    float4 a = mul(float4(p[0].p, 1), g_vp), b = mul(float4(p[1].p, 1), g_vp), c = mul(float4(p[2].p, 1), g_vp);
    // Conservative clip rejection includes displacement; keep patches crossing the near plane.
    float pad = max(g_geometry.z, 0.1) * max(g_proj.x, g_proj.y) * 2;
    bool outside = (max(a.w, max(b.w, c.w)) < -pad) ||
        (max(a.x + a.w, max(b.x + b.w, c.x + c.w)) < -pad) ||
        (max(a.w - a.x, max(b.w - b.x, c.w - c.x)) < -pad) ||
        (max(a.y + a.w, max(b.y + b.w, c.y + c.w)) < -pad) ||
        (max(a.w - a.y, max(b.w - b.y, c.w - c.y)) < -pad);
    f.edge[0] = outside ? 0 : EdgeTess(p[1].p, p[2].p);
    f.edge[1] = outside ? 0 : EdgeTess(p[2].p, p[0].p);
    f.edge[2] = outside ? 0 : EdgeTess(p[0].p, p[1].p);
    f.inside = max(f.edge[0], max(f.edge[1], f.edge[2]));
    return f;
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("WaterFactors")]
[maxtessfactor(64.0)]
PatchPoint HsWater(InputPatch<PatchPoint, 3> p, uint id : SV_OutputControlPointID) { return p[id]; }

#if defined(MML_WATER_WORLD_MATERIAL) || defined(MML_WATER_WORLD_SHORE)
#include "mml/gfx/waterline.hlsli"
static bool g_worldShoreUnknown=false;
float WorldShoreWeight(float3 base){
    if(length(base-g_eye.xyz)>g_worldPath.y)return 1; // declared finite contact region, independent of image bounds
    float2 contact=MmlQueryWaterlineLocal(base);
    if(contact.y!=0){g_worldShoreUnknown=true;return 1;}
    return smoothstep(.1,1,contact.x);
}
#endif
float ShoreWeight(float3 base) {
    if (g_water.z < 0.5 || abs(base.y - g_water.x) > 0.05) return 1;
    float2 uv = WaterReflectionUv(base, g_rvp);
    if (any(uv < 0) || any(uv > 1)) return 1;
    float z = dot(float4(base, 1), g_rdepth);
    if (z <= 0.05) return 1;
    float depth = g_reflDepth.SampleLevel(g_clamp, uv, 0);
    // The reflected wall's distance from the mean surface. Pin its cut foot, open water gets full waves.
    return WaterProjectedShoreWeight(z,depth,length(base-g_eye.xyz));
}

VOut WaterDomain(float3 bary,const OutputPatch<PatchPoint,3> patch,bool worldShore,bool previousMotion) {
    VOut o;
    float3 base = patch[0].p * bary.x + patch[1].p * bary.y + patch[2].p * bary.z;
    o.base = base;
    o.shore = ShoreWeight(base);
    o.shoreUnknown=0;
#if defined(MML_WATER_WORLD_SHORE)
    if(worldShore){o.shore=WorldShoreWeight(base);o.shoreUnknown=g_worldShoreUnknown?1:0;}
#endif
    float3 delta = 0;
    float dist = length(base - g_eye.xyz);
    // Filter wavelengths smaller than tessellated edges; fade geometry smoothly into the far surface.
    float spacing = clamp(dist * 0.006, 0.25, 16);
    [unroll] for (int c = 0; c < 3; c++) {
        float mip = max(log2(spacing * g_cascade[c] * 256), 0);
        delta += g_displacement.SampleLevel(g_wrap, float3(base.xz * g_cascade[c], c), mip).xyz;
    }
    float strength = g_water.y * g_geometry.x * g_geometry.y * o.shore;
    float3 p = base + float3(-delta.y * g_light.w, delta.x, -delta.z * g_light.w) * strength;
    o.pos = mul(float4(p, 1), g_vp);
    // The world and replacement geometry must sample the same subpixel location before temporal upscaling.
    o.pos.xy += g_temporal.xy * float2(2, -2) * g_size.zw * o.pos.w;
    o.world = p;
    o.previous = 0;
    if (previousMotion && g_temporal.z > 0.5 && g_temporal.w > 0.5) {
        float3 before = 0;
        float spacingBefore = clamp(length(base - g_previousEye.xyz) * 0.006, 0.25, 16);
        [unroll] for (int c = 0; c < 3; c++) {
            float mip = max(log2(spacingBefore * g_cascade[c] * 256), 0);
            before += g_previousDisplacement.SampleLevel(g_wrap, float3(base.xz * g_cascade[c], c), mip).xyz;
        }
        float3 previous = base + float3(-before.y * g_light.w, before.x, -before.z * g_light.w) * strength;
        o.previous = mul(float4(previous, 1), g_previousVP);
    }
    return o;
}
[domain("tri")]
VOut DsWater(PatchFactors f,float3 bary:SV_DomainLocation,const OutputPatch<PatchPoint,3> patch){return WaterDomain(bary,patch,false,true);}
#if defined(MML_WATER_WORLD_SHORE)
[domain("tri")]
VOut DsWaterWorld(PatchFactors f,float3 bary:SV_DomainLocation,const OutputPatch<PatchPoint,3> patch){return WaterDomain(bary,patch,true,true);}
[domain("tri")]
VOut DsWaterSeed(PatchFactors f,float3 bary:SV_DomainLocation,const OutputPatch<PatchPoint,3> patch){return WaterDomain(bary,patch,true,false);}
#endif

struct WaterPixel { float4 color : SV_Target0; float4 temporal : SV_Target1; float depth : SV_Depth; };
#if defined(MML_WATER_WORLD_MATERIAL)
#include "river_world_material.hlsli"
#else
#include "mml/gfx/scene_query.hlsli"
#endif
#include "river_lighting.hlsli"

// Explicit native geometry/material diagnostics. Production PsWater above still uses the accepted object path.
#if !defined(MML_WATER_WORLD_MATERIAL)
StructuredBuffer<float4> g_probeRay : register(t22);
struct WorldProbe {MmlRayHit hit;float4 uv,diffuse;uint4 attributes;float4 encoded,radiance,path,composite;};
RWStructuredBuffer<WorldProbe> g_probeHit : register(u0);
void WaterWorldProbe(bool blend) {
    float4 origin=g_probeRay[0],direction=g_probeRay[1];
    WorldProbe result=(WorldProbe)0;
    if(blend){MmlSceneRadiance scene=MmlTraceSceneRadiance(origin.xyz,direction.xyz,origin.w,direction.w,0);
        result.hit=scene.first;result.composite=float4(scene.color,1);result.path=float4(scene.status,scene.layers,scene.transmission,0);}
    else {if(g_probeRay[2].x>.5)result.hit=MmlSceneOpacityRay(origin.xyz,direction.xyz,origin.w,direction.w,0);
        else result.hit=MmlSceneGeometryRay(origin.xyz,direction.xyz,origin.w,direction.w);
        result.path=float4(result.hit.status,result.hit.status==1?1:0,result.hit.status==0?1:0,0);}
    result.attributes.x=0xffffffff;
    if(result.hit.status==1)result.attributes.y=MmlSurfaceAttributes(result.hit.primitive,result.hit.bary,result.uv,result.diffuse,result.attributes.x)?1:0;
    if(result.hit.status==1){uint material;result.attributes.z=MmlSurfaceEncoded(result.hit.primitive,result.hit.bary,0,result.encoded,material)?1:0;
        if(result.attributes.z!=0)result.radiance=float4(MmlNativeLinear(result.encoded.rgb),result.encoded.a);}
    if(!blend)result.composite=result.radiance;
    g_probeHit[0]=result;
}
[numthreads(1,1,1)]void CsWaterWorldRay(uint3 id:SV_DispatchThreadID){WaterWorldProbe(false);}
[numthreads(1,1,1)]void CsWaterWorldBlend(uint3 id:SV_DispatchThreadID){WaterWorldProbe(true);}
WaterPixel WaterGeometryView(VOut i,bool radianceView) {
    // Geometry diagnostics replace the entire color. Do not optimize the unused
    // complete illumination path merely to obtain its depth/motion outputs.
    WaterPixel pixel=(WaterPixel)0;
    float3 p=i.world,v=normalize(g_eye.xyz-p);
    float2 px=i.pos.xy;float z=dot(p-g_eye.xyz,g_fwd.xyz);bool repaired=false;
    WaterSurface s=WaterRipples(g_slopes,g_shape,g_wrap,i.base.xz,g_cascade.xyz,g_cascade.w,
                               g_water.y*g_geometry.x*i.shore,g_light.w);
    g_traceLevel=i.base.y;g_traceStrength=g_water.y*g_geometry.x*i.shore;
    if(dot(s.normal,v)<=.025 && g_geometry.y>.5) {
        WaterWaveHit visible=WaterTraceWaves(g_eye.xyz,-v,i.base.y,max(g_geometry.z,.05),length(g_eye.xyz-p)+2);
        if(visible.status==1){p=visible.p;s.normal=WaterTraceNormal(p);z=dot(p-g_eye.xyz,g_fwd.xyz);repaired=true;}
    }
    if(g_eye.y<=i.base.y || z>SceneDepth(px)+.02)discard;
    float4 clip=mul(float4(p,1),g_vp);pixel.depth=clip.z/clip.w;
    float4 previous=i.previous;
    if(repaired&&g_temporal.z>.5){
        float2 base=WaterTraceBase(p.xz);float3 before=0;
        [unroll]for(uint c=0;c<3;c++)before+=g_previousDisplacement.SampleLevel(g_wrap,float3(base*g_cascade[c],c),0).xyz;
        float3 q=float3(base.x,i.base.y,base.y)+float3(-before.y*g_light.w,before.x,-before.z*g_light.w)*g_traceStrength;
        previous=mul(float4(q,1),g_previousVP);
    }
    float2 motion=0;
    if(g_temporal.z>.5&&previous.w>1e-5){
        float2 previousPixel=(previous.xy/previous.w*float2(.5,-.5)+.5)*g_size.xy;
        motion=previousPixel-(px-g_temporal.xy);
    }
    pixel.temporal=float4(z,motion,1);
    // The resolved mean-facet first reflection; additional wave interactions/material integration follow later.
    float3 direction=reflect(-v,s.normal);
    if(radianceView){
        // Explicit raw incident-radiance preview: actual native object materials, then the native sky cube
        // only after a complete bounded miss. Unknown coverage/materials never become sky or an old image.
        // No Fresnel/body/foam, continued wave bounces, outgoing atmosphere or ray footprints in this view.
        MmlSceneRadiance scene=MmlTraceSceneRadiance(p,direction,.02,300,0);
        float3 radiance=float3(1,0,1);
        if(scene.status==1)radiance=scene.color;
        else if(scene.status==0){
            if(g_light.z==25)radiance=scene.color+scene.transmission*float3(.03,.08,.25);
            else if(g_environmentInfo.y>.5)
                radiance=scene.color+scene.transmission*g_environment.SampleLevel(g_clamp,direction,0).rgb;
        }
        pixel.color=float4(Enc(radiance),1);return pixel;
    }
    MmlRayHit hit;if(g_light.z==24)hit=MmlSceneOpacityRay(p,direction,.02,300,0);else hit=MmlSceneGeometryRay(p,direction,.02,300);
    float3 color=hit.status==0 ? float3(.03,.08,.25) : hit.status==2 ? float3(1,0,1) : float3(1,.3,0);
    if(hit.status==1)color=.2+.8*frac(float3(.6180339,.4142136,.7320508)*hit.instance);
    if((g_light.z==23||g_light.z==24) && hit.status==1){float4 encoded;uint material;
        color=MmlSurfaceEncoded(hit.primitive,hit.bary,0,encoded,material)?MmlNativeLinear(encoded.rgb):float3(1,0,1);}
    pixel.color=float4(g_light.z>=23?Enc(color):color,1);return pixel;
}
WaterPixel PsWaterGeometry(VOut i){return WaterGeometryView(i,false);}
WaterPixel PsWaterRadiance(VOut i){return WaterGeometryView(i,true);}

// Offline WARP probe of the actual shared optics used above; never dispatched by the mod.
[numthreads(8,1,1)]
void CsWaterOptics(uint3 id : SV_DispatchThreadID) {
    float4 result = 0;
    if (id.x == 0) result = float4(WaterFresnel(1),WaterFresnel(.5),WaterFresnel(0),1);
    else if (id.x == 1) result = float4(WaterFresnel(-.5),WaterFresnel(-.9),WaterFresnel(.5),
                                      WaterFacetVisibility(float3(0,1,0),float3(0,-1,0)));
    else if (id.x == 2) {
        [unroll] for (uint j=0;j<7;j++) {
            float w;
            float3 n=WaterFacet(0,float2(.04,.09),j,w);
            float2 slope=-n.xz/n.y;
            result += w*float4(1,slope.x,slope.x*slope.x,slope.y*slope.y);
        }
    } else if (id.x == 3) {
        float3 v=normalize(float3(.98,.2,0));
        float lit=0,meanLight=0,share=0,total=0,crossMoment=0;
        [unroll] for (uint j=0;j<7;j++) {
            float w;
            float3 n=WaterFacet(0,float2(.04,.04),j,w);
            float2 slope=-n.xz/n.y;
            crossMoment+=w*slope.x*slope.x*slope.y*slope.y;
            w*=WaterFacetVisibility(n,v);
            float3 ray=reflect(-v,n);
            float f=ray.y>0 ? WaterFresnel(dot(n,v)) : 0;
            // Synthetic bright sky/dark bank: radiance changes with direction, not a uniform white test.
            float env=ray.y>.25 ? 4 : .05;
            lit+=w*f*env; meanLight+=w*env; share+=w*f;total+=w;
        }
        result=float4(lit/total,WaterFresnel(v.y)*meanLight/total,share/total,crossMoment);
    } else if (id.x == 4) {
        [unroll] for (uint j=0;j<7;j++) {
            float w;
            float3 n=WaterFacet(float2(.2,-.1),0,j,w);
            result+=w*float4(n,1);
        }
    } else if (id.x == 5) {
        float4x4 vp=float4x4(1,0,0,0, 0,1,0,0, 0,0,1,1, 0,0,0,0);
        float4x4 inv=float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
        WaterHit hit=WaterFollowReflection(float3(0,0,1),normalize(float3(0,.3,1)),float2(.5,.5),vp,
                         float4(0,0,1,0),g_reflDepth,float2(0,.01),inv,100);
        result=float4(hit.t,hit.sky ? 1 : 0,hit.uv.y,hit.contact ? 1 : 0);
    } else if (id.x == 6) {
        float flat=0,crest=0;
        [unroll] for(uint j=0;j<16;j++) {
            flat=max(flat,WaterWhitecaps(1,float2(j*.45,j*.21),0));
            crest=max(crest,WaterWhitecaps(.3,float2(j*.45,j*.21),0));
        }
        result=float4(flat,crest,0,1);
    } else if (id.x == 7) {
        float3 ray=float3(0,0,1),p=0;
        float a=ApplyAtmosphereAfterTravel(1,p,ray,300,50,false).x;
        float b=ApplyAtmosphereAfterTravel(1,p,ray,300,250,false).x;
        float c=ApplyAtmosphereAfterTravel(1,p,ray,100,50,false).x;
        result=float4(a,b,c,1);
    }
    g_skyOutput[uint2(id.x,0)] = result;
}
#endif
