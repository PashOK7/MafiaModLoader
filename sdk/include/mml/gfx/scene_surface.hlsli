// Original primitive order is independent of acceleration-structure sorting. Barycentrics are (vertex1,vertex2).
#ifndef MML_SCENE_SURFACE_HLSLI
#define MML_SCENE_SURFACE_HLSLI
#include "mml/gfx/native_material.hlsli"
#include "mml/gfx/native_texture_pages.hlsli"
struct MmlSceneSurface {float4 uv0,uv1,uv2;uint4 colorMaterial;};
struct MmlSceneStage {uint4 color,alpha,map,sampling,extra;};
struct MmlSceneMaterial {MmlSceneStage stage0,stage1;uint4 info,blend,factor;};
StructuredBuffer<MmlSceneSurface> mml_surfaces:register(t26);
StructuredBuffer<MmlSceneMaterial> mml_materials:register(t27);
#ifndef MML_SURFACE_TEXTURE
#define MML_SURFACE_TEXTURE(primitive,state,uv,lod,valid) MmlTextureSample(state.map.x,state.map.y==0?uv.xy:uv.zw,lod,state.sampling.xy,state.sampling.z,state.sampling.w,state.extra.x,valid)
#endif
#ifndef MML_SCENE_STAGE_LOOP
#define MML_SCENE_STAGE_LOOP [unroll]
#endif
#ifndef MML_SURFACE_ENCODED_RESULT
#define MML_SURFACE_ENCODED_RESULT(color,material)
#endif
bool MmlSurfaceAttributes(uint primitive,float2 bary,out float4 uv,out float4 diffuse,out uint material){
    uv=diffuse=0;material=0xffffffff;uint count,stride;mml_surfaces.GetDimensions(count,stride);if(primitive>=count)return false;
    MmlSceneSurface surface=mml_surfaces[primitive];float3 w=float3(1-bary.x-bary.y,bary);
    uv=surface.uv0*w.x+surface.uv1*w.y+surface.uv2*w.z;
    diffuse=MmlNativeArgb(surface.colorMaterial.x)*w.x+MmlNativeArgb(surface.colorMaterial.y)*w.y+MmlNativeArgb(surface.colorMaterial.z)*w.z;
    material=surface.colorMaterial.w;return true;
}
bool MmlSurfaceEncodedAt(uint surface,float2 bary,float lod,uint instanceMaterial,out float4 color,out uint material){
    float4 uv,diffuse;color=0;
    if(!MmlSurfaceAttributes(surface,bary,uv,diffuse,material))return false;
    if(instanceMaterial!=0xffffffff)material=instanceMaterial;
    uint count,stride;mml_materials.GetDimensions(count,stride);if(material>=count)return false;
    MmlSceneMaterial m=mml_materials[material];
    if(m.factor.y==0||m.factor.z!=0||(m.blend.w&14u)!=0)return false; // explicit custom shader/GPU skin/fixed lighting provider needed
    color=diffuse;float4 factor=MmlNativeArgb(m.factor.x);bool valid=true;
    MML_SCENE_STAGE_LOOP for(uint stage=0;stage<2;stage++){
        MmlSceneStage s=m.stage0;if(stage==1)s=m.stage1;
        if(s.color.x==1||s.map.w==0)break;
        if(s.map.x>=0xfffffffe||s.map.y>1||s.map.z!=0)return false;
        float4 tex=MML_SURFACE_TEXTURE(surface,s,uv,lod,valid);
        color=MmlNativeStage(s.color,s.alpha,tex,color,diffuse,factor,true,valid);
        if(!valid)return false;
    }
    MML_SURFACE_ENCODED_RESULT(color,material);
    return true;
}
bool MmlSurfaceEncoded(uint primitive,float2 bary,float lod,out float4 color,out uint material){
    return MmlSurfaceEncodedAt(primitive,bary,lod,0xffffffff,color,material);
}
bool MmlNativeAlphaPass(uint function,float alpha,uint reference){
    // Native fixed-pipeline comparison is in alpha bytes. A reciprocal multiply can otherwise make an
    // exact 200/255 texture value compare LESS than the independently normalized reference200.
    alpha=floor(saturate(alpha)*255+.5);float refValue=float(reference);
    if(function==1)return false;if(function==2)return alpha<refValue;if(function==3)return alpha==refValue;
    if(function==4)return alpha<=refValue;if(function==5)return alpha>refValue;if(function==6)return alpha!=refValue;
    if(function==7)return alpha>=refValue;return function==8;
}
// Reject cutout candidates BEFORE nearest-hit acceptance. Missing material is unknown, never a sky hole.
uint MmlSurfaceOpacity(uint primitive,float2 bary,float lod){
    float4 color;uint material;if(!MmlSurfaceEncoded(primitive,bary,lod,color,material))return 2;
    MmlSceneMaterial m=mml_materials[material];
    if(m.info.x!=0){if(m.info.z<1||m.info.z>8)return 2;if(!MmlNativeAlphaPass(m.info.z,color.a,m.info.y))return 0;}
    if(m.blend.x!=0)return 2; // ordered blended contributions are a separate radiance continuation
    return 1;
}
uint MmlSurfaceLayer(uint primitive,float2 bary,float lod){
    float4 color;uint material;if(!MmlSurfaceEncoded(primitive,bary,lod,color,material))return 2;
    MmlSceneMaterial m=mml_materials[material];
    if(m.info.x!=0){if(m.info.z<1||m.info.z>8)return 2;if(!MmlNativeAlphaPass(m.info.z,color.a,m.info.y))return 0;}
    if(m.blend.x!=0&&(m.blend.y!=5||(m.blend.z!=2&&m.blend.z!=6)))return 2;
    return 1;
}
#endif
