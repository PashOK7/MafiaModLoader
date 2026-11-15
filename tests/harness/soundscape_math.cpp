// Functional contracts: volume, PCM, 3D room-dependent reflections, EAX decay and D3D8 restoration.
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "../../mods/examples/soundscape/acoustics.h"
#include "../../mods/examples/soundscape/pcm.h"
#include "../../mods/examples/soundscape/surfaces.h"
#include "mml/gfx/legacy_mesh.h"
namespace {
unsigned checks=0;
void Check(bool value,const char *name){if(!value)throw std::runtime_error(name);++checks;printf("ok %s\n",name);}
bool Near(float a,float b,float epsilon=.0001f){return std::abs(a-b)<epsilon;}
acoustic::Hit Box(V3 from,V3 to,float half){
    const V3 d=Norm(to-from);float best=Len(to-from);V3 normal{};bool hit=false;
    for(int axis=0;axis<3;axis++)for(int sign:{-1,1}){
        if(std::abs((&d.x)[axis])<1e-6f)continue;
        const float t=(half*sign-(&from.x)[axis])/(&d.x)[axis];
        if(t<=.001f||t>best)continue;
        const V3 p=from+d*t;
        if(std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)})>half+.001f)continue;
        best=t;normal={};(&normal.x)[axis]=static_cast<float>(-sign);hit=true;
    }
    return {from+d*best,normal,hit};
}
struct MockState { D3DMATRIX transforms[3];D3DVIEWPORT9 viewport;DWORD zwrite,blend,vs,ps; };
struct Mock { void **vtable;MockState state,saved;bool fault=false;unsigned deleted=0,draws=0; };
int TransformIndex(D3DTRANSFORMSTATETYPE t){return t==D3DTS_WORLD?0:t==D3DTS_VIEW?1:2;}
HRESULT __stdcall GetTransform(void *v,D3DTRANSFORMSTATETYPE t,D3DMATRIX *m){*m=static_cast<Mock *>(v)->state.transforms[TransformIndex(t)];return S_OK;}
HRESULT __stdcall SetTransform(void *v,D3DTRANSFORMSTATETYPE t,const D3DMATRIX *m){static_cast<Mock *>(v)->state.transforms[TransformIndex(t)]=*m;return S_OK;}
HRESULT __stdcall GetViewport(void *v,D3DVIEWPORT9 *p){*p=static_cast<Mock *>(v)->state.viewport;return S_OK;}
HRESULT __stdcall SetViewport(void *v,const D3DVIEWPORT9 *p){static_cast<Mock *>(v)->state.viewport=*p;return S_OK;}
HRESULT __stdcall Capture(void *v,DWORD){auto *m=static_cast<Mock *>(v);m->saved=m->state;return S_OK;}
HRESULT __stdcall Create(void *v,D3DSTATEBLOCKTYPE,DWORD *t){*t=1;return Capture(v,1);}
HRESULT __stdcall Apply(void *v,DWORD){auto *m=static_cast<Mock *>(v);m->state=m->saved;return S_OK;}
HRESULT __stdcall Delete(void *v,DWORD){static_cast<Mock *>(v)->deleted++;return S_OK;}
HRESULT __stdcall GetStream(void *,UINT,void **stream,UINT *stride){*stream=nullptr;*stride=0;return S_OK;}
HRESULT __stdcall SetStream(void *,UINT,void *,UINT){return S_OK;}
HRESULT __stdcall SetTexture(void *,DWORD,void *){return S_OK;}
HRESULT __stdcall SetStage(void *,DWORD,D3DTEXTURESTAGESTATETYPE,DWORD){return S_OK;}
HRESULT __stdcall SetState(void *v,D3DRENDERSTATETYPE t,DWORD x){
    auto &s=static_cast<Mock *>(v)->state;if(t==D3DRS_ZWRITEENABLE)s.zwrite=x;if(t==D3DRS_DESTBLEND)s.blend=x;return S_OK;
}
HRESULT __stdcall SetVS(void *v,DWORD x){static_cast<Mock *>(v)->state.vs=x;return S_OK;}
HRESULT __stdcall SetPS(void *v,DWORD x){static_cast<Mock *>(v)->state.ps=x;return S_OK;}
HRESULT __stdcall Draw(void *v,D3DPRIMITIVETYPE,UINT,const void *,UINT){
    auto *m=static_cast<Mock *>(v);++m->draws;
    if(m->fault)RaiseException(0xE0000011,0,0,nullptr);return S_OK;
}
bool FaultDraw(Mock *m,const mml_legacy::Batch *b){
    __try {mml_legacy::Draw(m,b,1);return false;}__except(EXCEPTION_EXECUTE_HANDLER){return true;}
}
void Renderer(){
    void *vt[100]{};Mock m{};m.vtable=vt;
    vt[37]=reinterpret_cast<void *>(SetTransform);vt[38]=reinterpret_cast<void *>(GetTransform);
    vt[40]=reinterpret_cast<void *>(SetViewport);vt[41]=reinterpret_cast<void *>(GetViewport);
    vt[50]=reinterpret_cast<void *>(SetState);vt[54]=reinterpret_cast<void *>(Apply);
    vt[55]=reinterpret_cast<void *>(Capture);vt[56]=reinterpret_cast<void *>(Delete);vt[57]=reinterpret_cast<void *>(Create);
    vt[61]=reinterpret_cast<void *>(SetTexture);vt[63]=reinterpret_cast<void *>(SetStage);
    vt[72]=reinterpret_cast<void *>(Draw);vt[76]=reinterpret_cast<void *>(SetVS);vt[88]=reinterpret_cast<void *>(SetPS);
    vt[83]=reinterpret_cast<void *>(SetStream);vt[84]=reinterpret_cast<void *>(GetStream);
    m.state.transforms[0]._11=7;m.state.transforms[1]._22=3;m.state.transforms[2]._44=4;
    m.state.viewport={11,12,800,600,0,1};m.state.zwrite=1;m.state.vs=123;m.state.ps=456;
    const MockState before=m.state;
    const mml_legacy::Vertex triangle[3]={{0,0,0,0xffffffff},{1,0,0,0xffffffff},{0,1,0,0xffffffff}};
    const mml_legacy::Batch b{triangle,3,D3DPT_TRIANGLELIST,mml_legacy::Additive};
    const auto r=mml_legacy::Draw(&m,&b,1);
    Check(r.primitives==1&&r.restored&&m.draws==1,"actual 3D primitive submitted");
    Check(memcmp(&before,&m.state,sizeof before)==0&&m.deleted==1,"D3D8 transforms/states/viewport restored");
    m.fault=true;
    Check(FaultDraw(&m,&b),"render fault propagated to caller");
    Check(memcmp(&before,&m.state,sizeof before)==0&&m.deleted==2,"D3D8 state restored on render fault");
}
}
int main(){
    try {
        Check(Near(acoustic::Gain(-2000),.1f),"20 dB gain attenuation");
        Check(acoustic::Light(0)==0,"muted energy has no visible floor");
        Check(acoustic::Light(.2f)>acoustic::Light(.02f)*1.7f,"tenfold level has a visible brightness difference");
        const auto roomSmall=acoustic::Trace(V3{0,0,0},V3{1,0,0},30,[](V3 a,V3 b){return Box(a,b,2);});
        const auto large=acoustic::Trace(V3{0,0,0},V3{1,0,0},30,[](V3 a,V3 b){return Box(a,b,8);});
        Check(Near(roomSmall.firstHit,2)&&Near(large.firstHit,8),"actual room size changes first reflection distance");
        const auto reflected=acoustic::At(roomSmall,3);
        Check(reflected.bounce==1&&Near(reflected.at.x,1),"front bounces back from the wall");
        const V3 up{0,1,0};
        const std::vector<acoustic::Facet> floor{
            {{0,0,0},{1,0,0},{0,0,1},up,true},
            {{1,0,0},{1,0,1},{0,0,1},up,false},
            {{2,0,0},{3,0,0},{2,0,1},up,false},
            {{1,0,0},{1,0,1},{1,1,0},{1,0,0},false}};
        Check(acoustic::ConnectedSurfaces(floor)==std::vector<size_t>{0,1},
            "missed floor triangle joins the patch without crossing a gap or corner");
        const float edge=acoustic::DirectVisibility(1.9f,2);
        Check(edge>0&&edge<1&&acoustic::DirectVisibility(2,2)==0&&acoustic::DirectVisibility(1,2)==1,
            "first wall fades the direct surface continuously");
        Check(acoustic::Direction(0,3,10,24).y<-.99f&&acoustic::Direction(10,3,10,24).y>.99f,"room scan includes floor and ceiling");
        const float start=acoustic::Tail(1,.3f,.3f,2,0,0,0,0);
        Check(start>.999f&&acoustic::Tail(1,.2f,.3f,2,0,0,0,0)==0,"EAX delay controls wet onset");
        Check(Near(acoustic::Tail(1,2.3f,.3f,2,0,0,0,0)/start,.001f,.00001f),"EAX RT60 actually decays by 60 dB");
        Check(acoustic::Tail(1,.8f,.3f,2,0,0,.25f,.7f)>acoustic::Tail(1,.8f,.3f,2,0,0,0,0),"EAX echo depth affects repeated energy");
        pcm::Clip c;c.valid=true;c.bits=16;c.channels=1;c.rate=8000;c.bytes=1600;c.data.resize(1600);
        for(unsigned i=0;i<800;i++){const int16_t n=i%2?16384:-16384;c.data[i*2]=n&255;c.data[i*2+1]=(n>>8)&255;}
        pcm::Window(c,0,true);Check(Near(c.rms,.5f)&&Near(c.peak,.5f),"actual PCM RMS and peak decoded");
        const float high=c.rms*.8f,low=c.rms*.1f;
        Check(Near(high/low,8)&&Near((high*high)/(low*low),64),"source volume changes amplitude and energy");
        c.data.clear();pcm::Window(c,0,true);Check(c.rms==0,"unreadable PCM cannot invent amplitude");
        Renderer();printf("%u PASSED\n",checks);return 0;
    }catch(const std::exception &e){printf("FAIL %s\n",e.what());return 1;}
}
