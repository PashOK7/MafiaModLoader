// Read-only CPU-skin -> native D3D8 draw diagnostic, for both game builds.
// D3D9 wrappers with buffer renaming should use scene_draw_observe/geometry instead.
// Uses owner-scoped host hook chains. No vertex-buffer lock, GPU readback or game writes.
// See knowledge/notes/skinning.md; CopySkinnedPositions checked in both disassemblies.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "mml/mml.h"
#include "mml/game/game.h"

namespace mml_draw_probe {
struct Point { float x=0,y=0,z=0; };
struct Sample {
    const void *visual=nullptr,*copied_mesh=nullptr;
    uint64_t frame=0,copy_frame=0,ms=0;
    unsigned vertices=0,range_vertices=0,base=0,first=0;
    bool matched=false;
    bool indexed=false; // true only when a consumer used actual indices, rather than the min/count range
    float world[16]{};
    Point center{},lo{},hi{};
};
using Callback=void(*)(const Sample &,void *);
using Select=bool(*)(const void *,void *);
class Probe {
    struct Copy {
        const void *mesh=nullptr,*vb=nullptr;
        unsigned first=0;
        uint64_t frame=0;
        std::vector<Point> points;
    };
    const MmlHost *h=nullptr;MmlMod *mod=nullptr;
    Callback callback=nullptr;Select select=nullptr;void *user=nullptr;
    std::vector<Copy> copies;
    const void *visual=nullptr;
    uintptr_t copyAt=0,groupAt=0,dipAt=0;
    uint64_t frame=0;
    bool active=false;
    using CopyFn=void *(__fastcall *)(void *,void *,uint8_t *,int,int);
    using GroupFn=void(__fastcall *)(void *,void *,uint8_t *);
    using DipFn=HRESULT(__stdcall *)(void *,D3DPRIMITIVETYPE,UINT,UINT,UINT,UINT);
    CopyFn copyNext=nullptr;GroupFn groupNext=nullptr;DipFn dipNext=nullptr;
    inline static Probe *instance=nullptr; // one observer per mod DLL; host chains different mods
    template<class T> T Read(const void *p,int offset){T out{};if(p)h->mem_read(reinterpret_cast<uintptr_t>(p)+offset,&out,sizeof out);return out;}
    template<class F> static F Slot(void *p,int i){return reinterpret_cast<F>((*reinterpret_cast<void ***>(p))[i]);}
    void Copied(void *mesh,uint8_t *destination,int stride,int lod){
        if(!active||lod<0||lod>=8||stride<12||stride>256||!destination||!select(mesh,user))return;
        const void *instanceMesh=mesh;
        for(int depth=0;depth<8&&(Read<unsigned>(mesh,0x1D8)&0x10000);depth++)mesh=Read<void *>(mesh,8);
        const void *rec=Read<void *>(mesh,0x1E4+lod*4),*data=Read<void *>(mesh,0x1E0);
        const void *level=Read<void *>(data,0x0C+lod*4),*skin=Read<void *>(rec,0x30);
        const unsigned all=Read<unsigned>(level,8),rigid=Read<unsigned>(skin,0x0C);
        if(!rec||!skin||all<=rigid||all>8192)return;
        const void *wrapper=Read<void *>(rec,0x10);
        const void *vb=Read<void *>(wrapper,game::Ls3d().vbD3d);
        const unsigned first=Read<unsigned>(rec,0x18);
        if(!vb||first>2000000)return;
        auto it=std::find_if(copies.begin(),copies.end(),[&](const Copy &c){return c.vb==vb&&c.first==first;});
        if(it==copies.end()){
            if(copies.size()>=64)it=std::min_element(copies.begin(),copies.end(),[](const Copy&a,const Copy&b){return a.frame<b.frame;});
            else {copies.emplace_back();it=copies.end()-1;}
        }
        it->mesh=instanceMesh;it->vb=vb;it->first=first;it->frame=frame;
        it->points.resize(all-rigid);
        const void *cache=Read<void *>(rec,0x34);
        const auto *src=Read<const uint8_t *>(cache,0);
        const int cacheStride=Read<int>(cache,game::Ls3d().vbStride);
        const unsigned cacheFirst=Read<unsigned>(rec,0x3C);
        if(!src||cacheStride<12||cacheStride>256||cacheFirst>2000000){it->points.clear();return;}
        // These are the locked system-memory positions the native copy just read. Reading its
        // destination instead may stall when the real Direct3D 8 buffer is in video memory.
        const auto source=reinterpret_cast<uintptr_t>(src)+size_t(cacheFirst)*cacheStride;
        for(unsigned i=0;i<it->points.size();i++){
            auto &p=it->points[i];
            if(!h->mem_read(source+size_t(i)*cacheStride,&p,sizeof p)||
                !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)){it->points.clear();break;}
        }
        if(!it->points.empty())++copiesKept;
    }
    void Draw(void *device,UINT minimum,UINT vertices){
        if(!active||!visual||!callback||!device||!vertices||vertices>200000)return;
        Sample s{};s.visual=visual;s.frame=frame;s.ms=GetTickCount64();s.range_vertices=vertices;
        using Transform=HRESULT(__stdcall *)(void *,D3DTRANSFORMSTATETYPE,void *);
        if(FAILED(Slot<Transform>(device,38)(device,D3DTS_WORLD,s.world)))return;
        void *vb=nullptr,*ib=nullptr;UINT stride=0,base=0;
        const HRESULT a=Slot<HRESULT(__stdcall *)(void *,UINT,void **,UINT *)>(device,84)(device,0,&vb,&stride);
        const HRESULT b=Slot<HRESULT(__stdcall *)(void *,void **,UINT *)>(device,86)(device,&ib,&base);
        s.base=base;s.first=base+minimum;
        // On native D3D8 the singlemesh LOD uses the engine's fixed pipeline. Require the mesh's
        // own copy, the bound buffer and the matching range before reporting any position.
        if(SUCCEEDED(a)&&SUCCEEDED(b)&&vb){
            for(auto it=copies.rbegin();it!=copies.rend();++it){
                if(it->mesh!=visual||it->vb!=vb||s.first<it->first||s.first>=it->first+it->points.size())continue;
                const unsigned begin=s.first-it->first,n=std::min<unsigned>(vertices,static_cast<unsigned>(it->points.size())-begin);
                s.vertices=n;s.copied_mesh=it->mesh;s.copy_frame=it->frame;s.matched=n>0;
                for(unsigned i=0;i<n;i++){
                    const Point p=it->points[begin+i];
                    const Point w{p.x*s.world[0]+p.y*s.world[4]+p.z*s.world[8]+s.world[12],
                        p.x*s.world[1]+p.y*s.world[5]+p.z*s.world[9]+s.world[13],
                        p.x*s.world[2]+p.y*s.world[6]+p.z*s.world[10]+s.world[14]};
                    if(!i)s.lo=s.hi=w;
                    s.center.x+=w.x;s.center.y+=w.y;s.center.z+=w.z;
                    s.lo.x=std::min(s.lo.x,w.x);s.lo.y=std::min(s.lo.y,w.y);s.lo.z=std::min(s.lo.z,w.z);
                    s.hi.x=std::max(s.hi.x,w.x);s.hi.y=std::max(s.hi.y,w.y);s.hi.z=std::max(s.hi.z,w.z);
                }
                if(n){s.center.x/=n;s.center.y/=n;s.center.z/=n;}break;
            }
        }
        if(vb)Slot<ULONG(__stdcall *)(void *)>(vb,2)(vb);
        if(ib)Slot<ULONG(__stdcall *)(void *)>(ib,2)(ib);
        callback(s,user);
    }
    // Keep every original call/convention unchanged. Diagnostic faults disable observation only.
    static void SafeCopy(Probe *p,void *m,uint8_t *d,int s,int l){__try{p->Copied(m,d,s,l);}__except(EXCEPTION_EXECUTE_HANDLER){p->active=false;p->faults++;}}
    static void SafeDraw(Probe *p,void *d,UINT m,UINT v){__try{p->Draw(d,m,v);}__except(EXCEPTION_EXECUTE_HANDLER){p->active=false;p->faults++;}}
    static void *__fastcall CopyHook(void *mesh,void *edx,uint8_t *dest,int stride,int lod){
        Probe *p=instance;void *result=p->copyNext(mesh,edx,dest,stride,lod);SafeCopy(p,mesh,dest,stride,lod);return result;
    }
    static void __fastcall GroupHook(void *mesh,void *edx,uint8_t *entry){
        Probe *p=instance;const void *previous=p->visual;
        p->visual=p->active&&p->select&&p->select(mesh,p->user)?mesh:nullptr;
        __try{p->groupNext(mesh,edx,entry);}__finally{p->visual=previous;}
    }
    static HRESULT __stdcall DipHook(void *device,D3DPRIMITIVETYPE type,UINT minimum,UINT vertices,UINT start,UINT count){
        Probe *p=instance;SafeDraw(p,device,minimum,vertices);return p->dipNext(device,type,minimum,vertices,start,count);
    }
public:
    unsigned faults=0;
    unsigned copiesKept=0;
    bool Begin(const MmlHost *host,MmlMod *owner,Select which,Callback receive,void *context){
        if(instance)return false;h=host;mod=owner;select=which;callback=receive;user=context;instance=this;
        const uintptr_t base=h->module_base("LS3DF.dll");
        copyAt=base+game::Ls3d().smCopySkinned;groupAt=base+game::Ls3d().fgRenderObject;
        if(!h->hook(mod,copyAt,reinterpret_cast<void *>(CopyHook),reinterpret_cast<void **>(&copyNext))||
           !h->hook(mod,groupAt,reinterpret_cast<void *>(GroupHook),reinterpret_cast<void **>(&groupNext))){End();return false;}
        return true;
    }
    bool Frame(void *device8){
        active=false;++frame;
        if(!device8)return false;
        const uintptr_t at=reinterpret_cast<uintptr_t>((*reinterpret_cast<void ***>(device8))[71]);
        if(at!=dipAt){if(dipAt)h->unhook(mod,dipAt);dipAt=at;
            if(!h->hook(mod,dipAt,reinterpret_cast<void *>(DipHook),reinterpret_cast<void **>(&dipNext))){dipAt=0;return false;}}
        active=faults==0;return active;
    }
    void AfterWorld(){active=false;}
    void Clear(){copies.clear();visual=nullptr;}
    void End(){active=false;if(h){if(dipAt)h->unhook(mod,dipAt);if(groupNext)h->unhook(mod,groupAt);if(copyNext)h->unhook(mod,copyAt);}
        copyNext=nullptr;groupNext=nullptr;dipNext=nullptr;dipAt=0;copies.clear();instance=nullptr;}
};
} // namespace mml_draw_probe
