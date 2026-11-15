#pragma once
namespace opacity_test {
inline void Run(ID3D11Device *device,ID3D11DeviceContext *context){
    using namespace mml::scene;using namespace mml::ray;
    MmlHost host{};host.size=sizeof host;host.scene_draw_observe=snapshot_test::Observe;host.scene_draw_geometry=snapshot_test::Geometry;host.scene_draw_collect=snapshot_test::Collect;host.scene_draw_collect_ex=snapshot_test::CollectEx;
    SnapshotBuilder builder;builder.Begin();assert(builder.Collect(&host,reinterpret_cast<MmlMod *>(1),{{-100,-100,-100},{100,100,100}}));
    auto snapshot=*builder.Current(builder.Cycle());snapshot.materialsCollected=true;
    const uint32_t pixels[]={0x00ff0000,0xff00ff00,0xff0000ff,0xffffff00};
    TextureSnapshot cutout;cutout.native=D3DFMT_A8R8G8B8;cutout.format=DXGI_FORMAT_B8G8R8A8_UNORM;
    TextureMip mip;mip.width=mip.height=2;mip.pitch=8;mip.rows=2;mip.bytes.resize(sizeof pixels);memcpy(mip.bytes.data(),pixels,sizeof pixels);cutout.mips.push_back(mip);
    auto solid=cutout;const uint32_t green=0xff00ff00;for(unsigned i=0;i<4;i++)memcpy(solid.mips[0].bytes.data()+i*4,&green,4);
    snapshot.textures={cutout,solid};
    for(unsigned i=0;i<2;i++){auto &m=snapshot.materials[i];m.textureIds[0]=i;m.alphaTest=i==0;m.alphaFunction=7;m.alphaReference=128;m.cull=1;
        auto &s=m.stages[0];s.color_op=s.alpha_op=2;s.color_arg1=s.alpha_arg1=2;s.color_arg2=s.alpha_arg2=0;
        s.address_u=s.address_v=1;s.min_filter=s.mag_filter=1;s.mip_filter=0;m.stages[1].color_op=1;}
    Placement front;front.mesh=0;front.id=1;front.world[14]=4;Placement back;back.mesh=1;back.id=2;back.world[14]=8;
    assert(snapshot.geometry.Place({front,back},{},snapshot.nativeFrame));
    const float probes[9][4]={{-.5f,-.5f,0,.001f},{0,0,1,20},{-.5f,.5f,0,.001f},{0,0,1,20},
        {.5f,-.5f,0,.001f},{0,0,1,20},{.5f,.5f,0,.001f},{0,0,1,20},{1,0,0,0}};
    ComPtr<ID3D11Buffer> input,output,read;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;ComPtr<ID3D11ComputeShader> shader;
    Hr(device->CreateComputeShader(kCsRayMaterials,sizeof kCsRayMaterials,nullptr,&shader));
    D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof probes;d.StructureByteStride=16;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{probes,0,0};Hr(device->CreateBuffer(&d,&initial,&input));Hr(device->CreateShaderResourceView(input.Get(),nullptr,&srv));
    d.ByteWidth=16*16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Hr(device->CreateBuffer(&d,nullptr,&output));Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&d,nullptr,&read));
    RadianceGpu gpu;
    auto run=[&](){assert(gpu.Upload(device,snapshot,1)&&gpu.BindCS(context,snapshot.cycle,1));auto *view=srv.Get();auto *out=uav.Get();
        context->CSSetShaderResources(22,1,&view);context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(1,1,1);
        out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);RadianceGpu::UnbindCS(context);context->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));std::array<std::array<float,4>,16> values;memcpy(values.data(),mapped.pData,sizeof values);context->Unmap(read.Get(),0);return values;};
    auto result=run();
    assert(result[2][0]==1&&result[2][1]==2&&result[0][0]==0&&result[0][1]==1&&result[0][2]==0);
    assert(result[6][0]==1&&result[6][1]==1&&result[4][0]==0&&result[4][1]==0&&result[4][2]==1);
    std::swap(snapshot.textures[0],snapshot.textures[1]);
    snapshot.materials[0].textureIds[0]=1;snapshot.materials[1].textureIds[0]=0;
    const auto reordered=run();
    assert(gpu.TexturesReused()&&memcmp(reordered.data(),result.data(),7*sizeof(result[0]))==0);
    // Dropping the front texture cannot turn its former index into a valid binding.
    snapshot.textures.pop_back();
    const auto subset=run();
    assert(gpu.TexturesReused()&&subset[2][0]==0&&subset[2][1]==float(UINT32_MAX));
    snapshot.textures={cutout,solid};
    snapshot.materials[1].textureIds[0]=1;
    snapshot.materials[0].textureIds[0]=UINT32_MAX-1;auto missing=run();assert(missing[2][0]==0&&missing[2][1]==float(UINT32_MAX));
    snapshot.materials[0].textureIds[0]=0;snapshot.materials[0].blend=1;auto blended=run();assert(blended[6][0]==0&&blended[6][1]==float(UINT32_MAX));
    std::puts("native cutouts: alpha rejects before nearest-hit acceptance, real hidden background, missing/blended data remain unknown: ALL PASSED");
}
}
