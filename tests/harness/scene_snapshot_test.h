#pragma once
#include "mml/scene_radiance_gpu.h"
#include "mml/scene_waterline.h"
#include <wrl/client.h>
#include <cassert>
namespace snapshot_test {
inline MmlSceneDrawFn observer=nullptr;
inline void *observerUser=nullptr;
inline const MmlSceneDraw *active=nullptr;
inline bool failCollection=false,refuseGeometry=false,repeatMesh=false,splitMaterial=false,clearCutout=false;
inline unsigned change=0,extra=0;inline float shift=0,firstVertexShift=0;
inline MmlSceneVertex vertices[4]{};
inline uint32_t indices[48]{};
inline void (*pendingWork)(void *)=nullptr;
inline void *pendingUser=nullptr;
inline uint64_t ticket=0;
inline uint64_t SubmitOwned(MmlMod *,void (*work)(void *),void *user){
    if(pendingWork)return 0;pendingWork=work;pendingUser=user;return ++ticket;
}
inline int JoinOwned(MmlMod *,uint64_t expected,int cancel){
    if(!pendingWork||expected!=ticket)return 0;
    auto work=pendingWork;pendingWork=nullptr;
    if(!cancel)work(pendingUser);
    pendingUser=nullptr;return !cancel;
}
inline int Observe(MmlMod *,MmlSceneDrawFn fn,void *user){observer=fn;observerUser=user;return 1;}
inline int Geometry(MmlMod *,const MmlSceneDraw *draw,MmlSceneGeometry *out){
    if(refuseGeometry||draw!=active||out->size<sizeof *out)return 0;
    out->vertices=4;out->vertex_stride=sizeof(MmlSceneVertex);out->vertex=vertices;out->triangles=repeatMesh?16:2;out->index=indices;out->source_fvf=0x242;
    static const MmlSceneMaterialDetail detail{D3DTOP_DISABLE,D3DBLENDOP_ADD,7,{{0,D3DTA_CURRENT,1,0,0},{0,D3DTA_CURRENT,1,0,0}}};
    if(clearCutout)out->material_detail=&detail;
    return 1;
}
inline int Collect(MmlMod *,const float *,const float *,MmlSceneCollection *out){
    if(!observer||failCollection)return 0;
    MmlSceneTextureStage stage[2]{};stage[0].texture=reinterpret_cast<void *>(0x1234);stage[0].color_op=5;
    if(clearCutout){stage[0].texture=nullptr;stage[0].color_op=stage[1].color_op=D3DTOP_DISABLE;}
    for(unsigned object=0;object<2+extra;object++){
        vertices[0]={{-1,-1,0},0xff102030,{0,0},{.1f,.2f}};vertices[1]={{1,-1,0},0xff405060,{1,0},{.3f,.4f}};
        vertices[2]={{-1,1,0},0xff708090,{0,1},{.5f,.6f}};vertices[3]={{1,1,0},0xffa0b0c0,{1,1},{.7f,.8f}};
        indices[0]=0;indices[1]=1;indices[2]=2;indices[3]=2;indices[4]=1;indices[5]=3;
        if(change==1)vertices[0].pos[0]-=.25f;
        if(change==2)vertices[0].uv1[0]+=.25f;
        if(change==3)vertices[0].color^=0xff00;
        if(change==4)std::swap(indices[1],indices[2]);
        if(object>=2)vertices[0].pos[0]-=object*.125f;
        if(object==0)vertices[0].pos[0]+=firstVertexShift;
        if(repeatMesh)for(unsigned i=6;i<48;i++)indices[i]=indices[i%6];
        MmlSceneDraw draw{};draw.size=sizeof draw;draw.flags=MML_SCENE_DRAW_COLLECTION;draw.frame=99;draw.visual=reinterpret_cast<void *>(0x1000+object);
        draw.world[0]=draw.world[5]=draw.world[10]=draw.world[15]=1;draw.world[12]=float(object*3);draw.world[14]=8;
        draw.world[12]+=shift;if(change==5)draw.cull=D3DCULL_CW;
        if(splitMaterial&&object==1)draw.alpha_reference=200;
        if(clearCutout){for(auto &vertex:vertices)vertex.color&=0xffffffu;draw.alpha_test=1;draw.alpha_reference=200;draw.alpha_function=7;}
        draw.stage_stride=sizeof(MmlSceneTextureStage);draw.stages=stage;active=&draw;observer(&draw,observerUser);active=nullptr;
    }
    for(auto &v:vertices)v.pos[0]=NAN; // callbacks must own geometry/attributes by now
    static MmlSceneMissing missing{};missing.lo[0]=missing.lo[1]=missing.lo[2]=-50;missing.hi[0]=missing.hi[1]=missing.hi[2]=-40;missing.reason=3;
    out->frame=99;out->selected=2+extra;out->draws=2+extra;out->missing_count=1;out->missing=&missing;return 1;
}
inline int CollectEx(MmlMod *mod,const float *lo,const float *hi,uint32_t flags,MmlSceneCollection *out){
    assert(flags==MML_SCENE_COLLECT_BILLBOARDS);return Collect(mod,lo,hi,out);
}
inline void Run(ID3D11Device *device,ID3D11DeviceContext *context){
    MmlHost host{};host.size=uint32_t(offsetof(MmlHost,scene_draw_collect_ex));host.scene_draw_observe=Observe;host.scene_draw_geometry=Geometry;host.scene_draw_collect=Collect;
    auto *mod=reinterpret_cast<MmlMod *>(1);mml::scene::SnapshotBuilder builder;
    const mml::ray::Bounds bounds{{-100,-100,-100},{100,100,100}};
    assert(!builder.Collect(&host,mod,bounds));
    const auto cycle=builder.Begin();assert(builder.Collect(&host,mod,bounds));const auto *s=builder.Current(cycle);
    assert(s&&s->nativeFrame==99&&s->draws==2&&s->sources.size()==2&&s->geometry.instances.size()==2&&s->surfaces.size()==4&&s->materials.size()==2);
    assert(!observer&&s->geometry.unknown.size()==1&&s->geometry.unknown[0].reason==3);
    for(const auto &t:s->geometry.triangles){assert(mml::ray::Finite(t.p));assert(t.flags==0);assert(s->surfaces[t.primitive].material==t.material);}
    assert(s->surfaces[0].uv[0][2]==.1f&&s->surfaces[0].color[0]==0xff102030);
    assert(!s->materials[0].stages[0].texture&&s->materials[0].textureKeys[0]==0x1234);
    assert(s->meshBuilt==1&&s->meshReused==1&&s->meshRevisions[0]==s->meshRevisions[1]);
    const auto original=s->meshRevisions[0];
    host.size=sizeof host;host.scene_draw_collect_ex=CollectEx;
    auto collect=[&](){builder.Begin();assert(builder.Collect(&host,mod,bounds));return builder.Current(builder.Cycle());};
    s=collect();assert(s->meshBuilt==0&&s->meshReused==2&&s->meshRevisions[0]==original);
    shift=12;s=collect();assert(s->meshBuilt==0&&s->meshRevisions[0]==original&&s->sources[0].world[12]==12);shift=0;
    for(change=1;change<=5;change++){
        s=collect();assert(s->meshBuilt==1&&s->meshReused==1&&s->meshRevisions[0]!=original);
        const auto changed=s->meshRevisions[0];s=collect();assert(s->meshBuilt==0&&s->meshRevisions[0]==changed);
        for(const auto &t:s->geometry.triangles)assert(s->surfaces[t.primitive].material==t.material);
    }
    change=0;s=collect();assert(s->meshBuilt==1&&s->meshRevisions[0]!=original); // original content retired, no stale lifetime
    builder.DropSnapshot();s=collect();assert(s->meshBuilt==0&&s->meshReused==2);
    builder.Begin();assert(!builder.Current(cycle));failCollection=true;assert(!builder.Collect(&host,mod,bounds)&&!builder.Current(builder.Cycle()));failCollection=false;
    refuseGeometry=true;assert(builder.Collect(&host,mod,bounds));s=builder.Current(builder.Cycle());assert(s&&s->unavailable==2&&s->geometry.instances.empty()&&s->geometry.unknown.size()==3);refuseGeometry=false;
    assert(s->cachedMeshBytes==0); // an unavailable source never resurrects its retired cached geometry
    assert(builder.Collect(&host,mod,bounds));builder.Invalidate();assert(!builder.Current(builder.Cycle()));builder.Release();
    s=collect();assert(s->meshBuilt==1&&s->meshReused==1);builder.Release();
    collect();auto owned=builder.Lease(builder.Cycle());std::weak_ptr<const mml::scene::Snapshot> prior=owned;owned.reset();
    builder.Begin();assert(prior.expired()); // a failed prior publication must not add a third full scene next frame
    assert(builder.Collect(&host,mod,bounds,false,true));
    s=builder.Current(builder.Cycle());
    assert(s&&s->instancedMeshes&&s->geometry.meshes.size()==1&&s->geometry.triangles.size()==2&&s->surfaces.size()==4);
    const auto &a=s->geometry.instances[0],&b=s->geometry.instances[1];
    assert(a.root==b.root&&a.id!=b.id&&(a.flags&mml::ray::InstanceAttributes)&&(b.flags&mml::ray::InstanceAttributes));
    for(const auto &t:s->geometry.triangles){
        assert(a.Material(t)==0&&b.Material(t)==1&&a.Primitive(t)+2==b.Primitive(t));
    }
    builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,false,true));
    s=builder.Current(builder.Cycle());
    assert(s->materials.size()==1&&s->primitiveCount==4&&s->surfaces.size()==2);
    assert(s->geometry.instances[0].pad==0&&s->geometry.instances[1].pad==0&&s->geometry.instances[1].pad1==2);
    splitMaterial=true;builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,false,true));
    s=builder.Current(builder.Cycle());
    assert(s->materials.size()==2&&s->geometry.instances[1].pad==1&&s->materials[1].alphaReference==200);
    splitMaterial=false;
    extra=16;firstVertexShift=-.0625f;
    builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true));
    auto previous=builder.Lease(builder.Cycle());
    const auto stableRoot=previous->geometry.instances[1].root;
    const auto oldRoot=previous->geometry.instances[0].root;
    assert(previous->liveBlasTriangles==36);
    firstVertexShift=-.125f;
    builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true));
    s=builder.Current(builder.Cycle());
    assert(s->geometry.triangles.size()==38&&s->liveBlasTriangles==36&&s->geometry.instances.size()==18);
    assert(s->geometry.instances[1].root==stableRoot&&s->geometry.instances[0].root!=oldRoot);
    assert(s->instancesReused==17);
    for(const auto &instance:s->geometry.instances)assert(instance.root!=oldRoot);
    previous.reset();builder.Release();
    extra=4;firstVertexShift=0;repeatMesh=true;
    builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true));
    auto serial=builder.Lease(builder.Cycle());
    builder.Release();builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,true));
    s=builder.Current(builder.Cycle());
    assert(s->workerMeshBuilds==5&&s->meshBuilt==5&&s->meshReused==1);
    auto equalBytes=[](const auto &a,const auto &b){return a.size()==b.size()&&memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0;};
    assert(equalBytes(s->geometry.nodes,serial->geometry.nodes)&&equalBytes(s->geometry.triangles,serial->geometry.triangles));
    assert(equalBytes(s->geometry.instances,serial->geometry.instances)&&equalBytes(s->surfaces,serial->surfaces));
    extra=1024;repeatMesh=false;
    builder.Release();builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true));
    serial=builder.Lease(builder.Cycle());
    builder.Release();builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,true));
    s=builder.Current(builder.Cycle());
    assert(s->workerInstances==513&&s->workerTopItems==513&&equalBytes(s->geometry.instances,serial->geometry.instances));
    assert(equalBytes(s->geometry.topNodes,serial->geometry.topNodes)&&s->geometry.instanceOrder==serial->geometry.instanceOrder);
    auto placed=builder.Lease(builder.Cycle());
    builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,true));
    s=builder.Current(builder.Cycle());
    assert(s->instancesReused==1026&&s->workerTopItems==0&&equalBytes(s->geometry.instances,placed->geometry.instances));
    assert(equalBytes(s->geometry.topNodes,placed->geometry.topNodes)&&s->geometry.instanceOrder==placed->geometry.instanceOrder);
    placed=builder.Lease(builder.Cycle());
    shift=3;builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,true));
    s=builder.Current(builder.Cycle());
    assert(s->instancesReused==0&&s->workerTopItems==513&&s->geometry.instances[0].lo.x==placed->geometry.instances[0].lo.x+3);
    shift=0;
    builder.Release();extra=16;firstVertexShift=-.0625f;
    for(unsigned warm=0;warm<3;warm++){
        builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,false,true,true));
    }
    const auto firstArchive=builder.Lease(builder.Cycle());
    const auto firstTriangles=firstArchive->geometry.Triangles();
    const std::vector<mml::ray::Triangle> firstBytes(firstTriangles.begin(),firstTriangles.end());
    mml::scene::RadianceGpu gpu;
    using Microsoft::WRL::ComPtr;
    auto views=[&](uint64_t cycle){
        assert(gpu.BindCS(context,cycle,1));
        std::array<ComPtr<ID3D11ShaderResourceView>,3> result;
        context->CSGetShaderResources(16,1,&result[0]);context->CSGetShaderResources(17,1,&result[1]);
        context->CSGetShaderResources(26,1,&result[2]);mml::scene::RadianceGpu::UnbindCS(context);return result;
    };
    assert(gpu.UploadOwned(device,firstArchive,1));const auto firstViews=views(firstArchive->cycle);
    firstVertexShift=-.125f;
    builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,false,true,true));
    const auto appended=builder.Lease(builder.Cycle());
    assert(appended->geometry.Triangles().data()==firstTriangles.data());
    assert(appended->geometry.Nodes().data()==firstArchive->geometry.Nodes().data());
    assert(appended->Surfaces().data()==firstArchive->Surfaces().data());
    assert(appended->geometry.Triangles().size()==firstTriangles.size()+2&&firstArchive->Surfaces().size()==firstTriangles.size());
    assert(equalBytes(firstArchive->geometry.Triangles(),firstBytes));
    assert(appended->geometry.instances[0].root!=firstArchive->geometry.instances[0].root);
    assert(gpu.UploadOwned(device,appended,1)&&gpu.AppendedBuffers()==3);
    const auto appendedViews=views(appended->cycle);
    auto verifyAppend=[&](unsigned index,const auto &old,const auto &now){
        ComPtr<ID3D11Resource> before,after;
        firstViews[index]->GetResource(&before);appendedViews[index]->GetResource(&after);
        assert(before.Get()==after.Get());
        D3D11_SHADER_RESOURCE_VIEW_DESC a{},b{};
        firstViews[index]->GetDesc(&a);appendedViews[index]->GetDesc(&b);
        assert(a.Buffer.NumElements==old.size()&&b.Buffer.NumElements==now.size());
        ComPtr<ID3D11Buffer> source,read;assert(SUCCEEDED(after.As(&source)));
        D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;
        assert(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&read)));
        context->CopyResource(read.Get(),source.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        assert(SUCCEEDED(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)));
        assert(!memcmp(mapped.pData,old.data(),old.size_bytes())&&!memcmp(mapped.pData,now.data(),now.size_bytes()));
        context->Unmap(read.Get(),0);
    };
    verifyAppend(0,firstArchive->geometry.Nodes(),appended->geometry.Nodes());
    verifyAppend(1,firstTriangles,appended->geometry.Triangles());
    verifyAppend(2,firstArchive->Surfaces(),appended->Surfaces());
    firstVertexShift=-.1875f;
    builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,false,true,true));
    const auto nextTail=builder.Lease(builder.Cycle());
    assert(nextTail->StableTriangles()==firstTriangles.size()&&nextTail->geometry.Triangles().size()==appended->geometry.Triangles().size());
    assert(equalBytes(nextTail->geometry.Triangles().first(firstTriangles.size()),firstBytes));
    assert(!equalBytes(nextTail->geometry.Triangles(),appended->geometry.Triangles()));
    assert(gpu.UploadOwned(device,nextTail,1)&&gpu.AppendedBuffers()==0);
    firstVertexShift=-.25f;
    extra=32;builder.Begin();assert(builder.Collect(&host,mod,bounds,false,true,false,true,true));
    const auto grown=builder.Lease(builder.Cycle());
    assert(grown->geometry.Triangles().data()!=firstTriangles.data()&&equalBytes(firstArchive->geometry.Triangles(),firstBytes));
    assert(grown->Surfaces().size()==grown->geometry.Triangles().size()&&grown->geometry.instances.size()==34);
    assert(gpu.UploadOwned(device,grown,1)&&gpu.AppendedBuffers()==0);
    builder.Release();assert(equalBytes(firstArchive->geometry.Triangles(),firstBytes));
    {
        mml::scene::SnapshotBuilder recycling;
        mml::scene::RadianceGpu currentGpu;
        std::shared_ptr<const mml::scene::Snapshot> pinned;
        std::vector<mml::ray::Triangle> pinnedBytes;
        bool reused=false,declinedPinned=false;
        extra=16;
        for(unsigned frame=0;frame<12;frame++){
            firstVertexShift=frame<3?0.f:-.0625f*float(frame-2);
            recycling.Begin();assert(recycling.Collect(&host,mod,bounds,false,true,false,true,true));
            const auto current=recycling.Lease(recycling.Cycle());
            assert(current&&current->recycledArchiveBytes<=32u*1024*1024);
            reused=reused||current->reusedPrefixBytes>0;
            if(frame==4){
                pinned=current;
                const auto triangles=current->geometry.Triangles();
                pinnedBytes.assign(triangles.begin(),triangles.end());
            }
            if(frame==6)declinedPinned=current->recycledArchiveBytes==0;
            if(pinned)assert(equalBytes(pinned->geometry.Triangles(),pinnedBytes));
            assert(currentGpu.UploadOwned(device,current,1));
        }
        assert(reused&&declinedPinned);
        recycling.Release();assert(equalBytes(pinned->geometry.Triangles(),pinnedBytes));
    }
    extra=32;
    firstVertexShift=0;
    host.scene_owned_submit=SubmitOwned;host.scene_owned_join=JoinOwned;
    struct PreparedResult {uint64_t cycle=0;unsigned draws=0,calls=0;} prepared;
    const auto prepare=[](const mml::scene::Snapshot &snapshot,void *context) noexcept {
        auto &out=*static_cast<PreparedResult *>(context);
        out.cycle=snapshot.cycle;out.draws=snapshot.draws;out.calls++;
    };
    const auto deferredCycle=builder.Begin();
    assert(builder.Collect(&host,mod,bounds,false,true,true,true,true,MML_SCENE_COLLECT_BILLBOARDS,true,prepare,&prepared,true));
    assert(pendingWork&&!prepared.calls&&!builder.Current(deferredCycle)&&builder.Complete()&&!pendingWork);
    assert(prepared.calls==1&&prepared.cycle==deferredCycle&&prepared.draws==34);
    const auto deferred=builder.Lease(deferredCycle);
    assert(deferred&&deferred->draws==34&&deferred->stagedDraws==34&&deferred->stagedMeshes>0&&deferred->stagedMeshPeakBytes<8u*1024*1024);
    mml::scene::SnapshotBuilder synchronous;
    synchronous.Begin();assert(synchronous.Collect(&host,mod,bounds,false,true,true,true,true));
    const auto control=synchronous.Lease(synchronous.Cycle());
    assert(equalBytes(deferred->geometry.Nodes(),control->geometry.Nodes()));
    assert(equalBytes(deferred->geometry.Triangles(),control->geometry.Triangles()));
    assert(equalBytes(deferred->geometry.instances,control->geometry.instances));
    assert(equalBytes(deferred->Surfaces(),control->Surfaces()));
    const auto cancelled=builder.Begin();
    assert(builder.Collect(&host,mod,bounds,false,true,true,true,true,MML_SCENE_COLLECT_BILLBOARDS,true,prepare,&prepared)&&pendingWork);
    builder.Invalidate();assert(!pendingWork&&!builder.Complete()&&!builder.Current(cancelled));
    assert(prepared.calls==1);
    extra=0;repeatMesh=false;
    clearCutout=true;builder.Begin();assert(builder.Collect(&host,mod,bounds,true,true,false,true,true));
    const auto cutout=builder.Lease(builder.Cycle());
    mml::scene::WaterlineModel shore;
    assert(cutout&&!cutout->Surfaces().empty()&&mml::scene::BuildWaterline(*cutout,{0},shore)&&shore.elements.empty());
    clearCutout=false;
}
}
