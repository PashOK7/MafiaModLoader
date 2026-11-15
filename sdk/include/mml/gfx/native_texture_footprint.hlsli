// Elliptical path footprint in ORIGINAL texels. Address every tap inside its original mip; adjacent
// raw allocations never bleed. Native sampler anisotropy bounds the major-axis quadrature.
#ifndef MML_NATIVE_TEXTURE_FOOTPRINT_HLSLI
#define MML_NATIVE_TEXTURE_FOOTPRINT_HLSLI
#include "mml/gfx/native_texture_pages.hlsli"
float4 MmlTextureSampleFootprintState(uint textureId,float2 uv,float2 dx,float2 dy,
                               uint2 address,uint minFilter,uint magFilter,uint mipFilter,uint maxAnisotropy,
                               uint maxMipLevel,float lodBias,inout bool valid){
    uint count,stride;mml_textures.GetDimensions(count,stride);
    if(textureId>=count){valid=false;return 0;}
    MmlTextureRecord record=mml_textures[textureId];if(record.count==0){valid=false;return 0;}
    if((asuint(lodBias)&0x7f800000u)==0x7f800000u){valid=false;return 0;}
    maxMipLevel=min(maxMipLevel,record.count-1);
    MmlMipRecord mip=mml_mips[record.first];float2 size=float2(mip.width,mip.height);
    dx*=size;dy*=size;
    if(any((asuint(dx)&0x7f800000u)==0x7f800000u)||any((asuint(dy)&0x7f800000u)==0x7f800000u)){valid=false;return 0;}
    float a=dx.x*dx.x+dy.x*dy.x;
    float b=dx.x*dx.y+dy.x*dy.y;
    float c=dx.y*dx.y+dy.y*dy.y;
    if(any((asuint(float3(a,b,c))&0x7f800000u)==0x7f800000u)){valid=false;return 0;}
    float spread=length(float2(.5*(a-c),b)),mean=.5*(a+c);
    float major=sqrt(max(mean+spread,0)),minor=sqrt(max(mean-spread,0));
    float lod=log2(max(major,1e-20));
    // Native NONE always samples original level0; MAXMIPLEVEL/bias affect mipmapped sampling only.
    float biased=mipFilter==0?lod:max(lod+lodBias,float(maxMipLevel));
    uint taps=1;float2 axis=0;lod=biased;
    if(minFilter==3&&biased>0){
        if(maxAnisotropy==0||maxAnisotropy>16){valid=false;return 0;}
        minor=max(max(minor,major/float(maxAnisotropy)),1);
        lod=mipFilter==0?0:clamp(log2(minor)+lodBias,float(maxMipLevel),float(record.count-1));
        float kernel=max(exp2(lod),major/float(maxAnisotropy));
        taps=max(1u,uint(ceil(min(major/kernel,float(maxAnisotropy)))));
        float angle=abs(a-c)+abs(b)>1e-20?.5*atan2(2*b,a-c):0;
        axis=float2(cos(angle),sin(angle))*sqrt(max(major*major-kernel*kernel,0))/size;
        minFilter=magFilter=2;
    }
    float4 color=0;
    [loop]for(uint i=0;i<taps;i++)color+=MmlTextureSample(textureId,uv+axis*((i+.5)/float(taps)-.5),lod,address,minFilter,magFilter,mipFilter,valid);
    return color/float(taps);
}
float4 MmlTextureSampleFootprint(uint textureId,float2 uv,float2 dx,float2 dy,
                               uint2 address,uint minFilter,uint magFilter,uint mipFilter,uint maxAnisotropy,inout bool valid){
    return MmlTextureSampleFootprintState(textureId,uv,dx,dy,address,minFilter,magFilter,mipFilter,maxAnisotropy,0,0,valid);
}
#endif
