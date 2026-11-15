// Batched colored world geometry through the SDK's borrowed Direct3D 8 device.
// Works on native D3D8 and D3D8 wrappers, without the D3D11 graphics bridge.
// COM slots checked against the pinned d3d8to9 IDirect3DDevice8 declaration.
// Each call keeps/restores all state, explicit transforms/viewport/stream zero,
// including on SEH unwind. No persistent GPU resource or state-block token.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstring>
#include "mml/mml.h"

namespace mml_legacy {
struct Vertex { float x,y,z; uint32_t color; };
enum : uint32_t { Additive=1, OnTop=2 };
struct Batch {
    const Vertex *vertices=nullptr;
    unsigned count=0;
    D3DPRIMITIVETYPE kind=D3DPT_TRIANGLELIST;
    uint32_t flags=0;
    const float *view=nullptr, *projection=nullptr;
    const D3DVIEWPORT9 *viewport=nullptr;
};
struct Result { HRESULT hr=S_OK; unsigned primitives=0; bool restored=false; };
template<class F> inline F Slot(void *d,int index) {
    return reinterpret_cast<F>((*reinterpret_cast<void ***>(d))[index]);
}
inline Result Draw(void *device8,const Batch *batches,unsigned count) {
    Result result{};
    if(!device8 || !batches || count>64){result.hr=E_INVALIDARG;return result;}
    for(unsigned k=0;k<count;k++) {
        const Batch &b=batches[k];
        const unsigned per=b.kind==D3DPT_TRIANGLELIST?3:b.kind==D3DPT_LINELIST?2:0;
        if(!per||b.count%per||b.count>300000||(!b.vertices&&b.count)){result.hr=E_INVALIDARG;return result;}
        for(unsigned i=0;i<b.count;i++)if(!std::isfinite(b.vertices[i].x)||
            !std::isfinite(b.vertices[i].y)||!std::isfinite(b.vertices[i].z)){result.hr=E_INVALIDARG;return result;}
    }
    using State=HRESULT(__stdcall *)(void *,D3DRENDERSTATETYPE,DWORD);
    using Transform=HRESULT(__stdcall *)(void *,D3DTRANSFORMSTATETYPE,const D3DMATRIX *);
    using GetTransform=HRESULT(__stdcall *)(void *,D3DTRANSFORMSTATETYPE,D3DMATRIX *);
    using Token=HRESULT(__stdcall *)(void *,DWORD);
    using Shader=HRESULT(__stdcall *)(void *,DWORD);
    void *d=device8,*stream=nullptr;
    DWORD token=0;
    UINT stride=0;
    D3DMATRIX world{},view{},projection{},identity{};
    D3DVIEWPORT9 viewport{};
    identity._11=identity._22=identity._33=identity._44=1;
    if(FAILED(Slot<GetTransform>(d,38)(d,D3DTS_WORLD,&world))||
       FAILED(Slot<GetTransform>(d,38)(d,D3DTS_VIEW,&view))||
       FAILED(Slot<GetTransform>(d,38)(d,D3DTS_PROJECTION,&projection))||
       FAILED(Slot<HRESULT(__stdcall *)(void *,D3DVIEWPORT9 *)>(d,41)(d,&viewport))){
        result.hr=E_FAIL;return result;
    }
    result.hr=Slot<HRESULT(__stdcall *)(void *,D3DSTATEBLOCKTYPE,DWORD *)>(d,57)(d,D3DSBT_ALL,&token);
    if(FAILED(result.hr)||!token)return result;
    result.hr=Slot<Token>(d,55)(d,token);
    if(FAILED(result.hr)){Slot<Token>(d,56)(d,token);return result;}
    Slot<HRESULT(__stdcall *)(void *,UINT,void **,UINT *)>(d,84)(d,0,&stream,&stride);
    __try {
        Slot<Transform>(d,37)(d,D3DTS_WORLD,&identity);
        Slot<Shader>(d,76)(d,D3DFVF_XYZ|D3DFVF_DIFFUSE);
        Slot<Shader>(d,88)(d,0);
        Slot<HRESULT(__stdcall *)(void *,DWORD,void *)>(d,61)(d,0,nullptr);
        auto stage=Slot<HRESULT(__stdcall *)(void *,DWORD,D3DTEXTURESTAGESTATETYPE,DWORD)>(d,63);
        stage(d,0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);stage(d,0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
        stage(d,0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);stage(d,0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
        stage(d,1,D3DTSS_COLOROP,D3DTOP_DISABLE);stage(d,1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
        auto rs=Slot<State>(d,50);
        rs(d,D3DRS_LIGHTING,FALSE);rs(d,D3DRS_FOGENABLE,FALSE);
        rs(d,D3DRS_CULLMODE,D3DCULL_NONE);rs(d,D3DRS_FILLMODE,D3DFILL_SOLID);
        rs(d,D3DRS_ALPHATESTENABLE,FALSE);rs(d,D3DRS_ALPHABLENDENABLE,TRUE);
        rs(d,D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);rs(d,D3DRS_ZWRITEENABLE,FALSE);
        rs(d,D3DRS_ZFUNC,D3DCMP_LESSEQUAL);rs(d,D3DRS_CLIPPING,TRUE);
        for(unsigned k=0;k<count;k++) {
            const Batch &b=batches[k];if(!b.count)continue;
            Slot<Transform>(d,37)(d,D3DTS_VIEW,b.view?reinterpret_cast<const D3DMATRIX *>(b.view):&view);
            Slot<Transform>(d,37)(d,D3DTS_PROJECTION,b.projection?reinterpret_cast<const D3DMATRIX *>(b.projection):&projection);
            Slot<HRESULT(__stdcall *)(void *,const D3DVIEWPORT9 *)>(d,40)(d,b.viewport?b.viewport:&viewport);
            rs(d,D3DRS_ZENABLE,(b.flags&OnTop)?D3DZB_FALSE:D3DZB_TRUE);
            rs(d,D3DRS_DESTBLEND,(b.flags&Additive)?D3DBLEND_ONE:D3DBLEND_INVSRCALPHA);
            const unsigned per=b.kind==D3DPT_TRIANGLELIST?3:2;
            for(unsigned i=0;i<b.count;i+=per*16000) {
                const unsigned n=(b.count-i<per*16000?b.count-i:per*16000)/per;
                result.hr=Slot<HRESULT(__stdcall *)(void *,D3DPRIMITIVETYPE,UINT,const void *,UINT)>(d,72)(
                    d,b.kind,n,b.vertices+i,sizeof(Vertex));
                if(FAILED(result.hr))__leave;
                result.primitives+=n;
            }
        }
    } __finally {
        const HRESULT restored=Slot<Token>(d,54)(d,token);
        Slot<Transform>(d,37)(d,D3DTS_WORLD,&world);Slot<Transform>(d,37)(d,D3DTS_VIEW,&view);
        Slot<Transform>(d,37)(d,D3DTS_PROJECTION,&projection);
        Slot<HRESULT(__stdcall *)(void *,const D3DVIEWPORT9 *)>(d,40)(d,&viewport);
        Slot<HRESULT(__stdcall *)(void *,UINT,void *,UINT)>(d,83)(d,0,stream,stride);
        if(stream)Slot<ULONG(__stdcall *)(void *)>(stream,2)(stream);
        Slot<Token>(d,56)(d,token);
        D3DMATRIX check{};
        Slot<GetTransform>(d,38)(d,D3DTS_WORLD,&check);
        result.restored=SUCCEEDED(restored)&&std::memcmp(&world,&check,sizeof world)==0;
    }
    return result;
}
} // namespace mml_legacy
