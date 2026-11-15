#pragma once
#include "mml/scene_radiance_gpu.h"
#include "shaders/CsRayAttributes.h"
#include "shaders/CsRayMaterials.h"
namespace attributes_test {
inline void Run(ID3D11Device *device,ID3D11DeviceContext *context){
    MmlHost host{};host.size=sizeof host;host.scene_draw_observe=snapshot_test::Observe;host.scene_draw_geometry=snapshot_test::Geometry;host.scene_draw_collect=snapshot_test::Collect;host.scene_draw_collect_ex=snapshot_test::CollectEx;
    mml::scene::SnapshotBuilder builder;builder.Begin();assert(builder.Collect(&host,reinterpret_cast<MmlMod *>(1),{{-100,-100,-100},{100,100,100}}));
    auto snapshot=*builder.Current(builder.Cycle());snapshot.materialsCollected=true;
    mml::scene::TextureSnapshot checker;checker.native=D3DFMT_A8R8G8B8;checker.format=DXGI_FORMAT_B8G8R8A8_UNORM;
    mml::scene::TextureMip mip;mip.width=mip.height=2;mip.pitch=8;mip.rows=2;
    const uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffff00};mip.bytes.resize(sizeof colors);memcpy(mip.bytes.data(),colors,sizeof colors);checker.mips.push_back(mip);snapshot.textures.push_back(checker);
    for(auto &m:snapshot.materials){m.textureIds[0]=0;auto &s=m.stages[0];s.alpha_op=4;s.alpha_arg1=2;s.alpha_arg2=0;s.color_arg1=2;s.color_arg2=0;
        s.address_u=s.address_v=1;s.min_filter=s.mag_filter=2;s.mip_filter=2;m.stages[1].color_op=1;}
    // Same local native mesh under a second nonuniform instance transform; immutable primitive IDs survive.
    mml::ray::Placement a;a.id=1;a.mesh=0;a.world[14]=8;
    auto b=a;b.id=2;b.mesh=1;b.world[0]=2;b.world[5]=3;b.world[10]=.5f;b.world[12]=3;b.world[14]=12;
    assert(snapshot.geometry.Place({a,b}, {},snapshot.nativeFrame));
    mml::scene::RadianceGpu gpu;assert(gpu.Upload(device,snapshot,1)&&gpu.Ready(snapshot.cycle,1));
    const float probes[8][4]={{-.2f,.3f,0,.001f},{0,0,1,20},{2.6f,.9f,0,.001f},{0,0,1,20},
        {.6f,-.2f,0,.001f},{0,0,1,20},{4.2f,-.6f,0,.001f},{0,0,1,20}};
    ComPtr<ID3D11Buffer> input,output,read;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;ComPtr<ID3D11ComputeShader> shader;
    Hr(device->CreateComputeShader(kCsRayAttributes,sizeof kCsRayAttributes,nullptr,&shader));
    D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof probes;d.StructureByteStride=16;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{probes,0,0};Hr(device->CreateBuffer(&d,&initial,&input));Hr(device->CreateShaderResourceView(input.Get(),nullptr,&srv));
    d.ByteWidth=16*16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Hr(device->CreateBuffer(&d,nullptr,&output));Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&d,nullptr,&read));
    assert(gpu.BindCS(context,snapshot.cycle,1));auto *view=srv.Get();auto *out=uav.Get();context->CSSetShaderResources(22,1,&view);context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(1,1,1);
    out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);mml::scene::RadianceGpu::UnbindCS(context);context->CopyResource(read.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));const auto *r=static_cast<const float (*)[4]>(mapped.pData);
    for(unsigned i=0;i<4;i++){
        assert(r[i*4][0]==1&&r[i*4][1]==(i%2?2:1)&&std::abs(r[i*4][3]-(i%2?12:8))<1e-5f);
        const float expectedUv[2]={i<2?.4f:.8f,i<2?.65f:.4f};
        assert(std::abs(r[i*4+1][0]-expectedUv[0])<1e-5f&&std::abs(r[i*4+1][1]-expectedUv[1])<1e-5f);
        const auto cpu=mml::ray::ReferenceGeometry(snapshot.geometry,snapshot.coverage,{probes[i*2][0],probes[i*2][1],0},{0,0,1},.001f,20);
        const auto &surface=snapshot.surfaces[cpu.primitive];float weights[]={1-cpu.bary[0]-cpu.bary[1],cpu.bary[0],cpu.bary[1]};
        const unsigned shift[]={16,8,0,24};for(unsigned c=0;c<4;c++){float value=0;for(unsigned v=0;v<3;v++)value+=float((surface.color[v]>>shift[c])&255)/255*weights[v];assert(std::abs(r[i*4+2][c]-value)<1e-5f);}
        const uint32_t color=colors[unsigned(expectedUv[1]*2)*2+unsigned(expectedUv[0]*2)];
        for(unsigned c=0;c<4;c++)assert(std::abs(r[i*4+3][c]-float((color>>shift[c])&255)/255)<1e-5f);
    }
    context->Unmap(read.Get(),0);
    shader.Reset();Hr(device->CreateComputeShader(kCsRayMaterials,sizeof kCsRayMaterials,nullptr,&shader));
    assert(gpu.BindCS(context,snapshot.cycle,1));out=uav.Get();context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(1,1,1);
    out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);mml::scene::RadianceGpu::UnbindCS(context);context->CopyResource(read.Get(),output.Get());
    Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));r=static_cast<const float (*)[4]>(mapped.pData);
    for(unsigned i=0;i<4;i++){
        assert(r[i*4+2][0]==1);
        const auto cpu=mml::ray::ReferenceGeometry(snapshot.geometry,snapshot.coverage,{probes[i*2][0],probes[i*2][1],0},{0,0,1},.001f,20);
        const auto &surface=snapshot.surfaces[cpu.primitive];const float weights[]={1-cpu.bary[0]-cpu.bary[1],cpu.bary[0],cpu.bary[1]};
        const float tex[2][3]={{.38f,.30f,.56f},{.34f,.90f,.03f}};const unsigned shifts[]={16,8,0,24};
        for(unsigned c=0;c<4;c++){float diffuse=0;for(unsigned v=0;v<3;v++)diffuse+=float((surface.color[v]>>shifts[c])&255)/255*weights[v];
            const float encoded=c==3?diffuse:(std::min)(1.0f,2*diffuse*tex[i/2][c]);assert(std::abs(encoded-r[i*4][c])<1e-5f);
            const float linear=c==3?encoded:encoded<=.04045f?encoded/12.92f:std::pow((encoded+.055f)/1.055f,2.4f);assert(std::abs(linear-r[i*4+1][c])<1e-5f);}
    }context->Unmap(read.Get(),0);
    auto broken=snapshot;broken.geometry.triangles[0].primitive=UINT32_MAX;assert(!gpu.Upload(device,broken,1)&&!gpu.BindCS(context,snapshot.cycle,1));
    assert(gpu.Upload(device,snapshot,2)&&!gpu.BindPS(context,snapshot.cycle,1)&&gpu.BindPS(context,snapshot.cycle,2));mml::scene::RadianceGpu::UnbindPS(context);gpu.Release();assert(!gpu.Ready(snapshot.cycle,2));
    // Static payloads share GPU storage but take the NEW immutable CPU lease. Keeping one old whole scene
    // per independently changing table would grow address-space use even with perfect GPU reuse.
    auto first=std::make_shared<const mml::scene::Snapshot>(snapshot);std::weak_ptr<const mml::scene::Snapshot> oldLease=first;
    assert(gpu.UploadOwned(device,first,3));auto changed=snapshot;changed.cycle++;changed.nativeFrame++;changed.geometry.frameId=changed.nativeFrame;
    auto second=std::make_shared<const mml::scene::Snapshot>(changed);first.reset();
    assert(gpu.UploadOwned(device,second,3)&&oldLease.expired()&&gpu.ReusedBuffers()==11&&gpu.UploadedBytes()==64&&gpu.TexturesReused());
    oldLease=second;for(auto &surface:changed.surfaces)surface.color[0]^=0x00ff00;changed.cycle++;changed.nativeFrame++;changed.geometry.frameId=changed.nativeFrame;
    auto third=std::make_shared<const mml::scene::Snapshot>(changed);second.reset();
    assert(gpu.UploadOwned(device,third,3)&&oldLease.expired()&&gpu.ReusedBuffers()==10&&gpu.UploadedBytes()==64+changed.surfaces.size()*sizeof(mml::scene::SurfaceTriangle));
    oldLease=third;third.reset();gpu.Release();assert(oldLease.expired());
    assert(gpu.Upload(device,snapshot,4));
    auto appended=snapshot;appended.cycle++;appended.nativeFrame++;appended.geometry.frameId=appended.nativeFrame;
    auto alternate=checker;alternate.mips[0].bytes[0]^=0x7f;
    appended.textures.insert(appended.textures.begin(),mml::scene::TextureRef(alternate));
    appended.textureBytes=2*sizeof colors;
    appended.materials[0].textureIds[0]=1; // existing texture moved to slot one in the current input
    assert(gpu.Upload(device,appended,4)&&!gpu.TexturesReused());
    assert(gpu.UploadedBytes()==64+2*sizeof(mml::scene::GpuNativeMaterial)+mml::scene::TexturePages::PageBytes-sizeof colors+
        2*(sizeof(mml::scene::TextureRecord)+sizeof(mml::scene::MipRecord)));
    mml::scene::RadianceGpu fresh;assert(fresh.Upload(device,appended,4));
    auto readMaterial=[&](mml::scene::RadianceGpu &publication){
        assert(publication.BindCS(context,appended.cycle,4));out=uav.Get();
        context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(1,1,1);
        out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);mml::scene::RadianceGpu::UnbindCS(context);
        context->CopyResource(read.Get(),output.Get());Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::array<uint8_t,16*16> data{};memcpy(data.data(),mapped.pData,data.size());context->Unmap(read.Get(),0);return data;
    };
    assert(readMaterial(gpu)==readMaterial(fresh));
    mml::scene::SnapshotBuilder producer;
    const mml::ray::Bounds bounds{{-100,-100,-100},{100,100,100}};
    auto collectOwned=[&](){
        producer.Begin();
        assert(producer.Collect(&host,reinterpret_cast<MmlMod *>(1),bounds,false,true));
        return producer.Lease(producer.Cycle());
    };
    const auto original=collectOwned();
    assert(gpu.UploadOwned(device,original,5));
    const auto same=collectOwned();
    auto altered=*same;
    altered.geometry.nodes.front().escape=0;
    assert(!gpu.Upload(device,altered,5));
    assert(gpu.UploadOwned(device,same,5)&&gpu.GeometryUploadedBytes()==64);
    assert(gpu.UploadOwned(device,same,6)&&gpu.GeometryUploadedBytes()>64);
    std::puts("texture bank append: GPU-copied prefix and reordered source IDs match a fresh bank: ALL PASSED");
    std::puts("scene publication: exact GPU payload reuse, changed attribute upload, current frame/generation and old CPU lease retirement: ALL PASSED");
    std::puts("scene attributes: original primitive/material/UV0/UV1/prelit colors and checker texels under nonuniform instances; coherent failure/lifecycle: ALL PASSED");
    std::puts("scene material: original opaque hit texture/prelighting and final linear decode: ALL PASSED");
}
}
