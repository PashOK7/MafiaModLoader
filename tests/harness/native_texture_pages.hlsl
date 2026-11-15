// Exercise the compact decoder used by the world-water provider against native
// typed textures; the default expanded decoder retains its prior baseline checks.
#define MML_NATIVE_TEXTURE_COMPACT 1
#include "mml/gfx/native_texture_footprint.hlsli"
struct Probe {uint4 info;float4 uvLod;uint4 filter;float4 samplingState;};
StructuredBuffer<Probe> probes:register(t0);
RWStructuredBuffer<float4> output:register(u0);
Texture2D<float4> references[9]:register(t1);
SamplerState referenceSamplers[12]:register(s0);
float4 ReferenceTexture(uint index,float2 uv,float lod,uint address,uint filter,uint mip){
    uint samplerIndex=(address-1)*4+(filter-1)*2+(mip==2?1:0);float4 value=0;
    [unroll]for(uint t=0;t<9;t++)if(t==index){
        [unroll]for(uint s=0;s<12;s++)if(s==samplerIndex)value=references[t].SampleLevel(referenceSamplers[s],uv,lod);
    }return value;
}
[numthreads(64,1,1)]void CsNativeTexture(uint3 id:SV_DispatchThreadID){
    uint count,stride;probes.GetDimensions(count,stride);if(id.x>=count)return;
    Probe p=probes[id.x];bool valid=true;
    if(p.info.w!=0){
        MmlTextureRecord record=mml_textures[p.info.x];MmlMipRecord mip=mml_mips[record.first];
        float2 size=float2(mip.width,mip.height);
        float4 filtered=MmlTextureSampleFootprintState(p.info.x,p.uvLod.xy,float2(p.uvLod.z/size.x,0),float2(0,p.uvLod.w/size.y),
            p.info.yz,p.filter.x,p.filter.y,p.filter.z,p.filter.w,uint(p.samplingState.y),p.samplingState.x,valid);
        float major=max(p.uvLod.z,p.uvLod.w),minor=max(min(p.uvLod.z,p.uvLod.w),major/float(p.filter.w));
        float lod=p.filter.z==0?0:max(log2(max(p.filter.x==3?minor:major,1))+p.samplingState.x,p.samplingState.y);
        output[id.x*2]=filtered;output[id.x*2+1]=valid?ReferenceTexture(p.info.x,p.uvLod.xy,lod,p.info.y,min(p.filter.x,2u),p.filter.z):float4(-1,-1,-1,-1);return;
    }
    float4 c=MmlTextureSample(p.info.x,p.uvLod.xy,p.uvLod.z,p.info.yz,p.filter.x,p.filter.y,p.filter.z,valid);
    output[id.x*2]=c;output[id.x*2+1]=valid?ReferenceTexture(p.info.x,p.uvLod.xy,p.uvLod.z,p.info.y,p.filter.x,p.filter.z):float4(-1,-1,-1,-1);
}
