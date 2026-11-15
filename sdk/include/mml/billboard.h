/* Native billboard rotation in original local coordinates. The observer is a world-ray origin transformed
 * by the ORIGINAL inverse world matrix. No main-camera matrix is retained. See notes/scene-billboards.md.
 * Required native modes only; false explicitly means unsupported or degenerate, never identity fallback. */
#pragma once
#include <cmath>
#include <cstdint>
namespace mml { namespace billboard {
inline bool Rotation(const float observer[3],uint32_t axis,uint32_t mode,float out[3][3]){
    if(!((axis==2&&mode==0)||(axis==1&&mode==1)))return false;
    for(unsigned i=0;i<3;i++)if(!std::isfinite(observer[i]))return false;
    const double x=observer[0],y=observer[1],z=observer[2],horizontal=std::hypot(x,z);
    if(horizontal==0)return false;
    const float a=float(x/horizontal),b=float(z/horizontal);
    out[0][0]=-b;out[0][1]=0;out[0][2]=a;
    if(mode==1){out[1][0]=0;out[1][1]=1;out[1][2]=0;out[2][0]=-a;out[2][1]=0;out[2][2]=-b;}
    else {const double radius=std::hypot(horizontal,y);const float e=float(horizontal/radius),f=float(y/radius);
        out[1][0]=-a*f;out[1][1]=e;out[1][2]=-b*f;
        out[2][0]=-a*e;out[2][1]=-f;out[2][2]=-b*e;}
    return true;
}
} }
