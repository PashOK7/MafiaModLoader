// Manual native mip sampling from owned raw pages. Decode only storage formats here; material stages retain
// encoded values. Filter taps address INSIDE the original mip, so adjacent allocations cannot bleed.
#ifndef MML_NATIVE_TEXTURE_PAGES_HLSLI
#define MML_NATIVE_TEXTURE_PAGES_HLSLI
struct MmlTextureRecord {uint first,count;uint2 pad;};
struct MmlMipRecord {uint offset,pitch,width,height,format;uint3 pad;};
StructuredBuffer<MmlTextureRecord> mml_textures:register(t23);
StructuredBuffer<MmlMipRecord> mml_mips:register(t24);
StructuredBuffer<uint> mml_texels:register(t25);
uint MmlTextureByte(uint address){return (mml_texels[address>>2]>>((address&3)*8))&255;}
uint MmlTextureHalf(uint address){return (mml_texels[address>>2]>>((address&3)*8))&65535;}
float3 MmlTexture565(uint value){return float3((value>>11)&31,(value>>5)&63,value&31)/float3(31,63,31);}
float4 MmlTextureTexel(MmlMipRecord mip,uint2 at){
    uint address=mip.offset+at.y*mip.pitch;uint value;
    if(mip.format<3){value=mml_texels[(address+at.x*4)>>2];float4 c=float4((value>>16)&255,(value>>8)&255,value&255,mip.format==2?255:value>>24)/255.0;return mip.format==1?c.bgra:c;}
    if(mip.format<6){value=MmlTextureHalf(address+at.x*2);
        if(mip.format==3)return float4(MmlTexture565(value),1);
        if(mip.format==4)return float4(float3((value>>10)&31,(value>>5)&31,value&31)/31.0,value>>15);
        return float4((value>>8)&15,(value>>4)&15,value&15,value>>12)/15.0;
    }
    address=mip.offset+(at.y/4)*mip.pitch+(at.x/4)*(mip.format==6?8:16);
    uint colorAddress=address+(mip.format==6?0:8),ends=mml_texels[colorAddress>>2];uint a=ends&65535,b=ends>>16;
    uint pixel=(at.y&3)*4+(at.x&3),selector=(mml_texels[(colorAddress+4)>>2]>>(pixel*2))&3;
    float3 ca=MmlTexture565(a),cb=MmlTexture565(b);float4 c=float4(selector==0?ca:selector==1?cb:0,1);
    if(selector>=2){if(a>b||mip.format!=6)c.rgb=selector==2?(2*ca+cb)/3:(ca+2*cb)/3;
        else if(selector==2)c.rgb=(ca+cb)*.5;else c=0;}
    if(mip.format==7)c.a=((mml_texels[(address+(pixel/8)*4)>>2]>>((pixel%8)*4))&15)/15.0;
    if(mip.format==8){uint a0=MmlTextureByte(address),a1=MmlTextureByte(address+1),bit=pixel*3;
        uint alphaBits=MmlTextureByte(address+2+bit/8)|(MmlTextureByte(address+3+bit/8)<<8),index=(alphaBits>>(bit%8))&7;
        c.a=index==0?a0/255.0:index==1?a1/255.0:a0>a1?((8-index)*a0+(index-1)*a1)/(7.0*255):
            index<6?((6-index)*a0+(index-1)*a1)/(5.0*255):index==6?0:1;
    }
    return c;
}
int MmlTextureAddress(int value,int size,uint mode){
    // Normalized UVs keep all point and bilinear taps in [-1,size].
    if(mode==3)return clamp(value,0,size-1);
    if(value<0)return mode==2?0:size-1;
    if(value>=size)return mode==2?size-1:0;
    return value;
}
float4 MmlTextureLevel(MmlMipRecord mip,float2 uv,uint2 address,uint filter){
    float2 pixel=uv*float2(mip.width,mip.height)-(filter==2?.5:0);int2 a=int2(floor(pixel));
    uint2 first=uint2(MmlTextureAddress(a.x,mip.width,address.x),MmlTextureAddress(a.y,mip.height,address.y));
#if defined(MML_NATIVE_TEXTURE_COMPACT)
    // One decoder call site, not four copies of every raw/BC format. Preserve
    // the original horizontal-then-vertical lerps, including point sampling.
    float4 texels[4] = {(float4)0,(float4)0,(float4)0,(float4)0};
    uint taps=filter==2?4u:1u;
    [loop]for(uint tap=0;tap<taps;tap++){
        uint2 at=uint2(MmlTextureAddress(a.x+int(tap&1),mip.width,address.x),
                       MmlTextureAddress(a.y+int(tap>>1),mip.height,address.y));
        texels[tap]=MmlTextureTexel(mip,at);
    }
    if(filter!=2)return texels[0];float2 t=frac(pixel);
    return lerp(lerp(texels[0],texels[1],t.x),lerp(texels[2],texels[3],t.x),t.y);
#else
    float4 c=MmlTextureTexel(mip,first);if(filter!=2)return c;
    uint2 second=uint2(MmlTextureAddress(a.x+1,mip.width,address.x),MmlTextureAddress(a.y+1,mip.height,address.y));float2 t=frac(pixel);
    return lerp(lerp(c,MmlTextureTexel(mip,uint2(second.x,first.y)),t.x),
        lerp(MmlTextureTexel(mip,uint2(first.x,second.y)),MmlTextureTexel(mip,second),t.x),t.y);
#endif
}
float4 MmlTextureSample(uint textureId,float2 uv,float lod,uint2 address,uint minFilter,uint magFilter,uint mipFilter,inout bool valid){
    // Anisotropic MIN is exact at zero footprint (MAG path); minification needs the path gradients at step20.
    if(any(address<1)||any(address>3)||minFilter<1||minFilter>3||(minFilter==3&&lod>0)||magFilter<1||magFilter>2||mipFilter>2||
       (asuint(uv.x)&0x7f800000u)==0x7f800000u||(asuint(uv.y)&0x7f800000u)==0x7f800000u||
       (asuint(lod)&0x7f800000u)==0x7f800000u){valid=false;return 0;}
    uint textureCount,recordStride;mml_textures.GetDimensions(textureCount,recordStride);
    if(textureId>=textureCount){valid=false;return 0;}
    MmlTextureRecord record=mml_textures[textureId];if(record.count==0){valid=false;return 0;}
    [unroll]for(uint axis=0;axis<2;axis++){uv[axis]=address[axis]==1?frac(uv[axis]):address[axis]==2?1-abs(frac(uv[axis]*.5)*2-1):saturate(uv[axis]);}
    uint filter=lod<=0?magFilter:minFilter;lod=clamp(mipFilter==0?0:lod,0,float(record.count-1));
    uint first=mipFilter==1?uint(floor(lod+.5)):uint(floor(lod));
#if defined(MML_NATIVE_TEXTURE_COMPACT)
    uint levels=mipFilter==2&&first+1<record.count?2u:1u;float4 c=0,other=0;
    [loop]for(uint level=0;level<levels;level++){
        float4 texelValue=MmlTextureLevel(mml_mips[record.first+first+level],uv,address,filter);
        if(level==0)c=texelValue;else other=texelValue;
    }
    return levels==2?lerp(c,other,frac(lod)):c;
#else
    float4 c=MmlTextureLevel(mml_mips[record.first+first],uv,address,filter);
    if(mipFilter==2&&first+1<record.count)c=lerp(c,MmlTextureLevel(mml_mips[record.first+first+1],uv,address,filter),frac(lod));return c;
#endif
}
#endif
