// Same required native rotations as mml/billboard.h. A reflected path supplies its own base-local observer.
#ifndef MML_BILLBOARD_HLSLI
#define MML_BILLBOARD_HLSLI
bool MmlBillboardRotation(float3 observer,uint axis,uint mode,out float3x3 rotation){
    rotation=0;
    if(!((axis==2&&mode==0)||(axis==1&&mode==1)))return false;
    if(any((asuint(observer)&0x7f800000u)==0x7f800000u))return false;
    float horizontal=length(observer.xz);if(horizontal==0)return false;
    float2 a=observer.xz/horizontal;
    rotation[0]=float3(-a.y,0,a.x);
    if(mode==1){rotation[1]=float3(0,1,0);rotation[2]=float3(-a.x,0,-a.y);}
    else {float2 e=float2(horizontal,observer.y)/length(observer);
        rotation[1]=float3(-a.x*e.y,e.x,-a.y*e.y);
        rotation[2]=float3(-a.x*e.x,-e.y,-a.y*e.x);}
    return true;
}
#endif
