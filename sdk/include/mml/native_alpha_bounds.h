/* Conservative zero-footprint native alpha coverage over a primitive section. Original owned mip0 texels,
 * native addressing/filter support and encoded fixed-stage alpha operations. A bound never invents coverage;
 * unsupported/partial input stays unknown. Useful for geometric fluid contact, independently of camera LOD. */
#pragma once
#include "mml/scene_snapshot.h"
#include <climits>
namespace mml { namespace scene {
struct AlphaInterval {float lo=0,hi=1;};
inline AlphaInterval AlphaWiden(float lo,float hi){
    lo=(std::clamp)(lo,0.0f,1.0f);hi=(std::clamp)(hi,0.0f,1.0f);
    if(lo==hi&&(lo==0||lo==1))return {lo,hi};
    for(unsigned n=0;n<4;n++){lo=std::nextafter(lo,-INFINITY);hi=std::nextafter(hi,INFINITY);}
    return {(std::max)(lo,0.0f),(std::min)(hi,1.0f)};
}
inline bool NativeAlphaTexel(const TextureSnapshot &texture,unsigned x,unsigned y,float &alpha){
    if(texture.mips.empty())return false;const auto &m=texture.mips[0];if(!m.width||!m.height||x>=m.width||y>=m.height||m.bytes.empty())return false;
    const auto byte=[&](size_t at,unsigned &v){if(at>=m.bytes.size())return false;v=m.bytes[at];return true;};
    const auto half=[&](size_t at,unsigned &v){unsigned a,b;if(!byte(at,a)||!byte(at+1,b))return false;v=a|(b<<8);return true;};
    unsigned value=0;
    if(texture.native==D3DFMT_X8R8G8B8||texture.native==D3DFMT_R5G6B5){alpha=1;return true;}
    if(texture.native==D3DFMT_A8R8G8B8||texture.native==D3DFMT_A8B8G8R8){if(!byte(size_t(y)*m.pitch+size_t(x)*4+3,value))return false;alpha=value/255.0f;return true;}
    if(texture.native==D3DFMT_A1R5G5B5||texture.native==D3DFMT_A4R4G4B4){if(!half(size_t(y)*m.pitch+size_t(x)*2,value))return false;alpha=texture.native==D3DFMT_A1R5G5B5?float(value>>15):float(value>>12)/15;return true;}
    if(texture.native!=D3DFMT_DXT1&&texture.native!=D3DFMT_DXT3&&texture.native!=D3DFMT_DXT5)return false;
    const size_t base=size_t(y/4)*m.pitch+size_t(x/4)*(texture.native==D3DFMT_DXT1?8:16);const unsigned pixel=(y&3)*4+(x&3);
    if(texture.native==D3DFMT_DXT1){unsigned a,b,index;if(!half(base,a)||!half(base+2,b)||!byte(base+4+pixel/4,index))return false;alpha=a<=b&&((index>>((pixel%4)*2))&3)==3?0.0f:1.0f;return true;}
    if(texture.native==D3DFMT_DXT3){if(!byte(base+pixel/2,value))return false;alpha=float((value>>((pixel%2)*4))&15)/15;return true;}
    unsigned a,b,low,high;const unsigned bit=pixel*3;
    if(!byte(base,a)||!byte(base+1,b)||!byte(base+2+bit/8,low)||!byte(base+3+bit/8,high))return false;
    const unsigned index=((low|(high<<8))>>(bit%8))&7;
    alpha=index==0?a/255.0f:index==1?b/255.0f:a>b?float((8-index)*a+(index-1)*b)/(7*255):
        index<6?float((6-index)*a+(index-1)*b)/(5*255):index==6?0.0f:1.0f;return true;
}
inline bool AlphaTexelSpan(float lo,float hi,unsigned size,unsigned mode,unsigned filter,int &first,int &last){
    if(!std::isfinite(lo)||!std::isfinite(hi)||lo>hi||!size||size>unsigned(INT_MAX/4)||mode<1||mode>3||filter<1||filter>2)return false;
    double a=lo,b=hi;if(mode==3){a=(std::clamp)(a,0.0,1.0);b=(std::clamp)(b,0.0,1.0);}
    else{const double period=mode==1?1.0:2.0;if(b-a>=period){first=0;last=int(size)-1;return true;}
        const double shift=std::floor(a/period)*period;a-=shift;b-=shift;}
    const double offset=filter==2?.5:0;
    first=int(std::floor(a*size-offset));last=int(std::floor(b*size-offset))+(filter==2?1:0);return true;
}
inline unsigned AlphaAddress(int value,unsigned size,unsigned mode){
    if(mode==3)return unsigned((std::clamp)(value,0,int(size)-1));
    const int period=int(mode==2?size*2:size);int index=value%period;if(index<0)index+=period;
    return mode==2&&index>=int(size)?unsigned(period-1-index):unsigned(index);
}
inline bool NativeTextureAlphaBounds(const TextureSnapshot &texture,const float lo[2],const float hi[2],const MmlSceneTextureStage &stage,AlphaInterval &result){
    if(texture.mips.empty())return false;const auto &m=texture.mips[0];int x0,x1,y0,y1;
    if(!AlphaTexelSpan(lo[0],hi[0],m.width,stage.address_u,stage.mag_filter,x0,x1)||!AlphaTexelSpan(lo[1],hi[1],m.height,stage.address_v,stage.mag_filter,y0,y1))return false;
    result={1,0};
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
        float alpha;if(!NativeAlphaTexel(texture,AlphaAddress(x,m.width,stage.address_u),AlphaAddress(y,m.height,stage.address_v),alpha))return false;
        result.lo=(std::min)(result.lo,alpha);result.hi=(std::max)(result.hi,alpha);
    }return result.lo<=result.hi;
}
inline bool NativeAlphaArgument(uint32_t argument,AlphaInterval texture,AlphaInterval current,AlphaInterval diffuse,AlphaInterval factor,AlphaInterval &out){
    if(argument&~63u)return false;switch(argument&15u){case 0:out=diffuse;break;case 1:out=current;break;case 2:out=texture;break;case 3:out=factor;break;default:return false;}
    if(argument&16u)out={1-out.hi,1-out.lo};return true;
}
inline bool NativeAlphaOperation(uint32_t op,AlphaInterval a,AlphaInterval b,float factor,AlphaInterval &out){
    if(op==2){out=a;return true;}if(op==3){out=b;return true;}
    if(op==4)out=AlphaWiden(a.lo*b.lo,a.hi*b.hi);
    else if(op==5)out=AlphaWiden(2*a.lo*b.lo,2*a.hi*b.hi);
    else if(op==7)out=AlphaWiden(a.lo+b.lo,a.hi+b.hi);
    else if(op==14)out=AlphaWiden(b.lo*(1-factor)+a.lo*factor,b.hi*(1-factor)+a.hi*factor);
    else return false;return true;
}
inline bool NativeSectionAlphaBounds(const Snapshot &source,uint32_t surfaceIndex,uint32_t materialIndex,
                                     const std::vector<std::array<float,2>> &section,AlphaInterval &result){
    const auto surfaces=source.Surfaces();
    if(section.empty()||surfaceIndex>=surfaces.size()||materialIndex>=source.materials.size())return false;
    const auto &surface=surfaces[surfaceIndex];const auto &material=source.materials[materialIndex];
    if((material.flags&14u)||material.unsupported)return false;
    float uvLo[4]={INFINITY,INFINITY,INFINITY,INFINITY},uvHi[4]={-INFINITY,-INFINITY,-INFINITY,-INFINITY};AlphaInterval diffuse{1,0};
    for(const auto &bary:section){
        const float w[3]={1-bary[0]-bary[1],bary[0],bary[1]};float alpha=0;
        for(unsigned v=0;v<3;v++)alpha+=float(surface.color[v]>>24)/255*w[v];
        diffuse.lo=(std::min)(diffuse.lo,alpha);diffuse.hi=(std::max)(diffuse.hi,alpha);
        for(unsigned axis=0;axis<4;axis++){float uv=0;for(unsigned v=0;v<3;v++)uv+=surface.uv[v][axis]*w[v];
            uvLo[axis]=(std::min)(uvLo[axis],uv);uvHi[axis]=(std::max)(uvHi[axis],uv);}
    }
    diffuse=AlphaWiden(diffuse.lo,diffuse.hi);result=diffuse;const float fa=float(material.factor>>24)/255;const AlphaInterval factor{fa,fa};
    for(unsigned i=0;i<2;i++){
        const auto &stage=material.stages[i];if(stage.color_op==1||material.textureKeys[i]==0)break;
        if(stage.coordinate>1||stage.transform_flags!=0||material.textureIds[i]>=source.textures.size())return false;
        const auto *texture=TextureData(source.textures[material.textureIds[i]]);if(!texture)return false;
        float lo[2],hi[2];for(unsigned a=0;a<2;a++){
            lo[a]=uvLo[stage.coordinate*2+a];hi[a]=uvHi[stage.coordinate*2+a];
            for(unsigned ulp=0;ulp<4;ulp++){lo[a]=std::nextafter(lo[a],-INFINITY);hi[a]=std::nextafter(hi[a],INFINITY);}
        }
        AlphaInterval tex,a,b;if(!NativeTextureAlphaBounds(*texture,lo,hi,stage,tex)||
            !NativeAlphaArgument(stage.alpha_arg1,tex,result,diffuse,factor,a)||!NativeAlphaArgument(stage.alpha_arg2,tex,result,diffuse,factor,b))return false;
        if(!NativeAlphaOperation(stage.alpha_op,a,b,fa,result))return false;
    }return true;
}
inline bool NativeSectionAlphaBounds(const Snapshot &source,uint32_t primitive,const std::vector<std::array<float,2>> &section,AlphaInterval &result){
    const auto surfaces=source.Surfaces();
    return primitive<surfaces.size()&&NativeSectionAlphaBounds(source,primitive,surfaces[primitive].material,section,result);
}
// 0 no surviving coverage, 1 every point passes, 2 partial/unknown. Native comparisons are in alpha bytes.
inline unsigned NativeAlphaCoverage(AlphaInterval bounds,uint32_t function,uint32_t reference){
    if(function==1)return 0;if(function==8)return 1;if(reference>255)return 2;
    const unsigned lo=unsigned(std::floor((std::clamp)(bounds.lo,0.0f,1.0f)*255+.5f)),hi=unsigned(std::floor((std::clamp)(bounds.hi,0.0f,1.0f)*255+.5f));
    switch(function){
        case 1:return 0;case 2:return hi<reference?1:lo>=reference?0:2;
        case 3:return lo==reference&&hi==reference?1:reference<lo||reference>hi?0:2;
        case 4:return hi<=reference?1:lo>reference?0:2;
        case 5:return lo>reference?1:hi<=reference?0:2;
        case 6:return reference<lo||reference>hi?1:lo==reference&&hi==reference?0:2;
        case 7:return lo>=reference?1:hi<reference?0:2;case 8:return 1;default:return 2;
    }
}
} }
